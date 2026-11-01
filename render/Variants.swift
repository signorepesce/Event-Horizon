import Foundation
import AppKit
import AVFoundation
import CoreImage
import CoreImage.CIFilterBuiltins
import Metal

func fail(_ message: String) -> Never {
    FileHandle.standardError.write(Data((message + "\n").utf8))
    exit(1)
}

guard CommandLine.arguments.count >= 3 else { fail("usage: variants <folder> <width>x<height>[@fps] …") }
let folder = URL(fileURLWithPath: CommandLine.arguments[1])
let master = folder.appendingPathComponent("Gargantua-7680x4800.mp4")
guard FileManager.default.fileExists(atPath: master.path) else { fail("Missing \(master.path); make it with: make render") }
let targets: [(Int, Int, Int?)] = CommandLine.arguments.dropFirst(2).compactMap {
    let parts = $0.split(separator: "@")
    let pair = parts[0].split(separator: "x")
    guard pair.count == 2, let w = Int(pair[0]), let h = Int(pair[1]) else { return nil }
    let rate = parts.count > 1 ? Int(parts[1]) : nil
    return (w, h, rate)
}
guard !targets.isEmpty else { fail("Give at least one size, e.g. 3072x1920 or 3072x1920@30") }

guard let device = MTLCreateSystemDefaultDevice() else { fail("No Metal GPU was found.") }
let ci = CIContext(mtlDevice: device, options: [.workingColorSpace: NSNull(), .cacheIntermediates: false])

func encode(width: Int, height: Int, rate: Int?) async throws {
    let asset = AVURLAsset(url: master)
    guard let track = try await asset.loadTracks(withMediaType: .video).first else {
        fail("The master has no video track")
    }
    let source = try await track.load(.naturalSize)
    let frameRate = try await track.load(.nominalFrameRate)
    let sourceFps = Int(frameRate.rounded())
    let fps = rate ?? sourceFps
    guard fps > 0, fps <= sourceFps, sourceFps % fps == 0 else {
        fail("\(fps) fps must divide the master's \(sourceFps) fps exactly")
    }
    let stride = sourceFps / fps
    let scale = Double(width) / source.width
    guard abs(scale - Double(height) / source.height) < 0.001 else {
        fail("\(width)x\(height): aspect ratio differs from the master \(source)")
    }

    let reader = try AVAssetReader(asset: asset)
    let output = AVAssetReaderTrackOutput(track: track, outputSettings: [
        kCVPixelBufferPixelFormatTypeKey as String: kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange,
        kCVPixelBufferMetalCompatibilityKey as String: true,
        kCVPixelBufferIOSurfacePropertiesKey as String: [:],
    ])
    output.alwaysCopiesSampleData = false
    reader.add(output)

    let url = folder.appendingPathComponent("Gargantua-\(width)x\(height)-\(fps)fps.mp4")
    try? FileManager.default.removeItem(at: url)
    let writer = try AVAssetWriter(outputURL: url, fileType: .mp4)
    let input = AVAssetWriterInput(mediaType: .video, outputSettings: [
        AVVideoCodecKey: AVVideoCodecType.hevc,
        AVVideoWidthKey: width,
        AVVideoHeightKey: height,
        AVVideoCompressionPropertiesKey: [
            AVVideoAverageBitRateKey: Int(Double(width * height) * Double(fps) * (fps > 30 ? 0.028 : 0.04)),
            AVVideoMaxKeyFrameIntervalKey: fps * 2,
            AVVideoExpectedSourceFrameRateKey: fps,
        ],
        AVVideoColorPropertiesKey: [
            AVVideoColorPrimariesKey: AVVideoColorPrimaries_ITU_R_709_2,
            AVVideoTransferFunctionKey: AVVideoTransferFunction_ITU_R_709_2,
            AVVideoYCbCrMatrixKey: AVVideoYCbCrMatrix_ITU_R_709_2,
        ],
    ])
    input.expectsMediaDataInRealTime = false
    let adaptor = AVAssetWriterInputPixelBufferAdaptor(assetWriterInput: input, sourcePixelBufferAttributes: [
        kCVPixelBufferPixelFormatTypeKey as String: kCVPixelFormatType_32BGRA,
        kCVPixelBufferWidthKey as String: width,
        kCVPixelBufferHeightKey as String: height,
        kCVPixelBufferMetalCompatibilityKey as String: true,
        kCVPixelBufferIOSurfacePropertiesKey as String: [:],
    ])
    writer.add(input)
    guard writer.startWriting() else { fail("Writer: \(String(describing: writer.error))") }
    writer.startSession(atSourceTime: .zero)
    guard reader.startReading() else { fail("Reader: \(String(describing: reader.error))") }

    let lanczos = CIFilter.lanczosScaleTransform()
    lanczos.scale = Float(scale)
    lanczos.aspectRatio = 1
    let crop = CGRect(x: 0, y: 0, width: width, height: height)
    let started = Date()
    var frames = 0
    var sourceIndex = -1

    while let sample = output.copyNextSampleBuffer() {
        sourceIndex += 1
        if sourceIndex % stride != 0 { continue }
        autoreleasepool {
            guard let pixels = CMSampleBufferGetImageBuffer(sample) else { fail("Frame \(frames) has no image") }
            while !input.isReadyForMoreMediaData && writer.status == .writing { usleep(2000) }
            guard writer.status == .writing else { fail("Writer: \(String(describing: writer.error))") }

            lanczos.inputImage = CIImage(cvPixelBuffer: pixels).clampedToExtent()
            guard let scaled = lanczos.outputImage else { fail("Lanczos") }

            var buffer: CVPixelBuffer?
            guard let pool = adaptor.pixelBufferPool,
                  CVPixelBufferPoolCreatePixelBuffer(nil, pool, &buffer) == kCVReturnSuccess,
                  let destination = buffer else { fail("Pixel buffer pool") }
            ci.render(scaled.cropped(to: crop), to: destination, bounds: crop, colorSpace: nil)
            let time = CMTime(value: Int64(frames), timescale: Int32(fps))
            guard adaptor.append(destination, withPresentationTime: time) else {
                fail("Append: \(String(describing: writer.error))")
            }
            frames += 1
            if frames % 90 == 0 {
                print("  \(width)x\(height): frame \(frames), \(Int(Date().timeIntervalSince(started)))s")
                fflush(stdout)
            }
        }
    }
    guard reader.status == .completed else { fail("Reader: \(String(describing: reader.error))") }
    input.markAsFinished()
    await writer.finishWriting()
    guard writer.status == .completed else { fail("Finish: \(String(describing: writer.error))") }

    let result = AVURLAsset(url: url)
    guard let resultTrack = try await result.loadTracks(withMediaType: .video).first else { fail("\(url.lastPathComponent) has no video track") }
    let size = try await resultTrack.load(.naturalSize)
    let duration = try await result.load(.duration)
    let generator = AVAssetImageGenerator(asset: result)
    generator.requestedTimeToleranceBefore = .zero
    generator.requestedTimeToleranceAfter = .zero
    func pixels(_ frame: Int) async throws -> [UInt8] {
        let image = try await generator.image(at: CMTime(value: Int64(frame), timescale: Int32(fps))).image
        let rep = NSBitmapImageRep(cgImage: image)
        return Array(UnsafeBufferPointer(start: rep.bitmapData, count: rep.bytesPerRow * rep.pixelsHigh))
    }
    func difference(_ a: [UInt8], _ b: [UInt8]) -> Double {
        zip(a, b).reduce(0.0) { $0 + Double(abs(Int($1.0) - Int($1.1))) } / Double(a.count)
    }
    let first = try await pixels(0), second = try await pixels(1), last = try await pixels(frames - 1)
    let bytes = (try? FileManager.default.attributesOfItem(atPath: url.path)[.size] as? Int) ?? nil
    print(String(format: "Done %@: %d×%d, %d frames, %.1f s, %.1f MB — adjacent frames %.3f, loop seam %.3f",
                 url.lastPathComponent, Int(size.width), Int(size.height), frames, duration.seconds,
                 Double(bytes ?? 0) / 1_048_576, difference(first, second), difference(last, first)))
    fflush(stdout)
}

let done = DispatchSemaphore(value: 0)
Task {
    do {
        for (w, h, rate) in targets { try await encode(width: w, height: h, rate: rate) }
    } catch {
        print("Error: \(error)")
        exit(1)
    }
    done.signal()
}
done.wait()
