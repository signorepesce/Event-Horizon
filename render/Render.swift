import Foundation
import Metal
import CoreVideo
import CoreImage
import AVFoundation
import VideoToolbox

func fail(_ message: String) -> Never {
    FileHandle.standardError.write(Data((message + "\n").utf8))
    exit(1)
}

let arguments = CommandLine.arguments
guard arguments.count == 5, let width = Int(arguments[2]), let height = Int(arguments[3]), let seconds = Int(arguments[4]),
      width >= 16, height >= 16, seconds >= 0 else {
    fail("usage: render <folder> <width> <height> <seconds>")
}
let folder = URL(fileURLWithPath: arguments[1])
let fps = 120
guard let device = MTLCreateSystemDefaultDevice(), let queue = device.makeCommandQueue() else { fail("No Metal GPU was found.") }
print("GPU: \(device.name); \(width)×\(height)")
let shader = try String(contentsOf: URL(fileURLWithPath: #filePath).deletingLastPathComponent()
    .appendingPathComponent("Gargantua.metal"), encoding: .utf8)
let lib = try device.makeLibrary(source: shader, options: nil)
func pipeline(_ name: String) throws -> MTLComputePipelineState {
    try device.makeComputePipelineState(function: lib.makeFunction(name: name)!)
}
let geometry = try pipeline("geometry")
let clouds = try pipeline("clouds")
let downsample = try pipeline("downsample")
let blur = try pipeline("blur")
let composite = try pipeline("composite")
func texture(_ w: Int, _ h: Int, _ format: MTLPixelFormat) -> MTLTexture {
    let d = MTLTextureDescriptor.texture2DDescriptor(pixelFormat: format, width: w, height: h, mipmapped: false)
    d.usage = [.shaderRead, .shaderWrite]
    return device.makeTexture(descriptor: d)!
}
let map = texture(width, height, .rgba32Float)
let radiance = texture(width, height, .rgba16Float)
let small = texture(width/4, height/4, .rgba16Float)
let bloomA = texture(width/4, height/4, .rgba16Float)
let bloomB = texture(width/4, height/4, .rgba16Float)
func encode(_ cb: MTLCommandBuffer, _ p: MTLComputePipelineState, _ ts: [MTLTexture], _ w: Int, _ h: Int, _ parameters: SIMD4<Float>) {
    let e = cb.makeComputeCommandEncoder()!
    e.setComputePipelineState(p)
    for (i,t) in ts.enumerated() { e.setTexture(t,index:i) }
    var parameters = parameters
    e.setBytes(&parameters,length:MemoryLayout<SIMD4<Float>>.stride,index:0)
    e.dispatchThreads(MTLSize(width:w,height:h,depth:1),threadsPerThreadgroup:MTLSize(width:16,height:16,depth:1))
    e.endEncoding()
}
let setup = queue.makeCommandBuffer()!
encode(setup,geometry,[map],width,height,SIMD4(0,0,0,0))
setup.commit(); setup.waitUntilCompleted()
if let error = setup.error { fail("Geometry: \(error)") }
print("Lensing map ready")
var cache: CVMetalTextureCache?
CVMetalTextureCacheCreate(nil,nil,device,nil,&cache)
let attributes: [String:Any] = [kCVPixelBufferPixelFormatTypeKey as String:kCVPixelFormatType_ARGB2101010LEPacked,kCVPixelBufferWidthKey as String:width,kCVPixelBufferHeightKey as String:height,kCVPixelBufferMetalCompatibilityKey as String:true,kCVPixelBufferIOSurfacePropertiesKey as String:[:]]
var pool: CVPixelBufferPool?
CVPixelBufferPoolCreate(nil,nil,attributes as CFDictionary,&pool)
let context = CIContext(mtlDevice:device)
let cs = CGColorSpace(name:CGColorSpace.sRGB)!
let step = 2*Float.pi/Float((seconds > 0 ? seconds : 24)*fps)
func render(_ phase: Float, save: String? = nil) -> CVPixelBuffer {
    var pixel: CVPixelBuffer?
    CVPixelBufferPoolCreatePixelBuffer(nil,pool!,&pixel)
    let buffer = pixel!
    var cvt: CVMetalTexture?
    CVMetalTextureCacheCreateTextureFromImage(nil,cache!,buffer,nil,.bgr10a2Unorm,width,height,0,&cvt)
    let target = CVMetalTextureGetTexture(cvt!)!
    let cb = queue.makeCommandBuffer()!
    encode(cb,clouds,[map,radiance],width,height,SIMD4(phase,step,0,0))
    encode(cb,downsample,[radiance,small],width/4,height/4,SIMD4(0,0,0,0))
    encode(cb,blur,[small,bloomA],width/4,height/4,SIMD4(1,0,0,0))
    encode(cb,blur,[bloomA,bloomB],width/4,height/4,SIMD4(0,1,0,0))
    encode(cb,composite,[radiance,bloomB,target],width,height,SIMD4(phase,0,0,0))
    cb.commit(); cb.waitUntilCompleted()
    if let error = cb.error { fail("Render: \(error)") }
    if let save = save {
        do { try context.writePNGRepresentation(of:CIImage(cvPixelBuffer:buffer),to:folder.appendingPathComponent(save),format:.RGBA8,colorSpace:cs) }
        catch { fail("Saving \(save): \(error.localizedDescription)") }
    }
    return buffer
}
if seconds == 0 {
    for (i,p) in [Float(0),Float.pi*0.5,Float.pi].enumerated() { _ = render(p,save:"preview-\(i).png") }
    print("Three preview frames saved")
    exit(0)
}
let output = folder.appendingPathComponent("Gargantua-\(width)x\(height)-\(fps)fps.mp4")
let writer = try AVAssetWriter(outputURL:output,fileType:.mp4)
let input = AVAssetWriterInput(mediaType:.video,outputSettings:[AVVideoCodecKey:AVVideoCodecType.hevc,AVVideoWidthKey:width,AVVideoHeightKey:height,AVVideoCompressionPropertiesKey:[AVVideoProfileLevelKey:kVTProfileLevel_HEVC_Main10_AutoLevel as String,AVVideoAverageBitRateKey:min(160_000_000,width*height*fps/2),AVVideoMaxKeyFrameIntervalKey:fps*2,AVVideoExpectedSourceFrameRateKey:fps],AVVideoColorPropertiesKey:[AVVideoColorPrimariesKey:AVVideoColorPrimaries_ITU_R_709_2,AVVideoTransferFunctionKey:AVVideoTransferFunction_ITU_R_709_2,AVVideoYCbCrMatrixKey:AVVideoYCbCrMatrix_ITU_R_709_2]])
input.expectsMediaDataInRealTime = false
let adaptor = AVAssetWriterInputPixelBufferAdaptor(assetWriterInput:input,sourcePixelBufferAttributes:attributes)
writer.add(input)
guard writer.startWriting() else { fail("Writer start: \(String(describing:writer.error))") }
writer.startSession(atSourceTime:.zero)
let start = Date()
for frame in 0..<(seconds*fps) {
    autoreleasepool {
        while !input.isReadyForMoreMediaData && writer.status == .writing { Thread.sleep(forTimeInterval:0.005) }
        guard writer.status == .writing else { fail("Writer: \(String(describing:writer.error))") }
        let pixel = render(Float(frame)*step)
        guard adaptor.append(pixel,withPresentationTime:CMTime(value:Int64(frame),timescale:Int32(fps))) else { fail("Append: \(String(describing:writer.error))") }
    }
    if frame % fps == 0 { print("Frame \(frame)/\(seconds*fps), elapsed \(Int(Date().timeIntervalSince(start)))s"); fflush(stdout) }
}
input.markAsFinished()
let done = DispatchSemaphore(value:0)
writer.finishWriting { done.signal() }
done.wait()
guard writer.status == .completed else { fail("Finish: \(String(describing:writer.error))") }
print("Saved \(output.path)")
