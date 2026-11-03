import AppKit
import AVFoundation
import VideoToolbox
import ServiceManagement
import CoreImage
import CoreImage.CIFilterBuiltins
import IOKit.ps
import UniformTypeIdentifiers
import ImageIO

let debugLog = ProcessInfo.processInfo.environment["EVENT_HORIZON_DEBUG"] != nil
func log(_ message: @autoclosure () -> String) {
    guard debugLog else { return }
    FileHandle.standardError.write(Data((message() + "\n").utf8))
}

func supportFolder() -> URL {
    let manager = FileManager.default
    if let path = Bundle.main.object(forInfoDictionaryKey: "EventHorizonData") as? String {
        let local = URL(fileURLWithPath: path, isDirectory: true)
        if manager.fileExists(atPath: local.deletingLastPathComponent().path) {
            try? manager.createDirectory(at: local, withIntermediateDirectories: false)
            if manager.isWritableFile(atPath: local.path) { return local }
        }
    }
    let root = manager.urls(for: .applicationSupportDirectory, in: .userDomainMask).first
        ?? URL(fileURLWithPath: NSHomeDirectory()).appendingPathComponent("Library/Application Support", isDirectory: true)
    let base = root.appendingPathComponent("Event Horizon", isDirectory: true)
    try? manager.createDirectory(at: base, withIntermediateDirectories: true)
    return base
}

enum Pref {
    static let speed = "speed"
    static let contentKind = "contentKind"
    static let contentPath = "contentPath"
    static let fit = "fit"
    static let pauseWhenCovered = "pauseWhenCovered"
    static let halfSpeedOnBattery = "halfSpeedOnBattery"
    static let halfSpeedInLowPower = "slowOnLowPower"

    static func register() {
        UserDefaults.standard.register(defaults: [
            speed: 1.0,
            contentKind: "",
            contentPath: "",
            fit: "fill",
            pauseWhenCovered: true,
            halfSpeedOnBattery: false,
            halfSpeedInLowPower: false,
        ])
    }

    static var chosenSpeed: Double { wp_speed_clamp(number(speed)) }
    static var prefs: wp_prefs {
        wp_prefs(speed: chosenSpeed, fit: Fit.current.core, pause_when_covered: flag(pauseWhenCovered),
                 half_speed_on_battery: flag(halfSpeedOnBattery), half_speed_in_low_power: flag(halfSpeedInLowPower))
    }
    static func flag(_ key: String) -> Bool { UserDefaults.standard.bool(forKey: key) }
    static func number(_ key: String) -> Double { UserDefaults.standard.double(forKey: key) }
    static func text(_ key: String) -> String { UserDefaults.standard.string(forKey: key) ?? "" }
    static func set(_ key: String, _ value: Any?) { UserDefaults.standard.set(value, forKey: key) }
}

enum Login {
    static let registered = "loginBuild"

    static var build: String {
        guard let url = Bundle.main.executableURL,
              let info = try? FileManager.default.attributesOfItem(atPath: url.path),
              let date = info[.modificationDate] as? Date, let size = info[.size] as? Int else { return "" }
        return "\(Int(date.timeIntervalSince1970))-\(size)"
    }

    static var enabled: Bool { SMAppService.mainApp.status == .enabled }

    static func set(_ on: Bool) throws {
        try? SMAppService.mainApp.unregister()
        guard on else { return }
        try SMAppService.mainApp.register()
        Pref.set(registered, build)
    }

    static func refresh() {
        let status = SMAppService.mainApp.status
        log("login item: status \(status.rawValue)")
        guard status != .notRegistered, status == .notFound || Pref.text(registered) != build else { return }
        do { try set(true) } catch { log("login item: \(error.localizedDescription)") }
        log("login item: renewed, status \(SMAppService.mainApp.status.rawValue)")
    }
}

enum Media {
    static let imageTypes: Set<String> = ["jpg", "jpeg", "png", "heic", "heif", "tiff", "tif", "bmp", "gif", "webp"]
}

enum Fit: String {
    case fill, fit

    static var current: Fit { Fit(rawValue: Pref.text(Pref.fit)) ?? .fill }

    var videoGravity: AVLayerVideoGravity {
        switch self {
        case .fill: return .resizeAspectFill
        case .fit: return .resizeAspect
        }
    }
    var core: wp_fit {
        switch self {
        case .fill: return WP_FIT_FILL
        case .fit: return WP_FIT_FIT
        }
    }
    var layerGravity: CALayerContentsGravity {
        switch self {
        case .fill: return .resizeAspectFill
        case .fit: return .resizeAspect
        }
    }
}

enum Background: Equatable {
    case video(URL)
    case image(URL)

    var url: URL {
        switch self {
        case .video(let url), .image(let url): return url
        }
    }
    var isVideo: Bool { if case .video = self { return true }; return false }
}

extension wp_rect {
    init(_ rect: CGRect) { self.init(x: rect.origin.x, y: rect.origin.y, w: rect.width, h: rect.height) }
}

extension CGRect {
    init(_ rect: wp_rect) { self.init(x: rect.x, y: rect.y, width: rect.w, height: rect.h) }
}

func uptime() -> Double { ProcessInfo.processInfo.systemUptime }

struct Library {
    let urls: [URL]
    let tiers: [wp_tier]

    static var variantsFolder: URL {
        let folder = supportFolder().appendingPathComponent("variants", isDirectory: true)
        try? FileManager.default.createDirectory(at: folder, withIntermediateDirectories: true)
        return folder
    }

    static func forgetStale(made sources: [URL]) {
        let manager = FileManager.default
        let stamp = sources.filter { var tier = wp_tier(); return wp_tier_parse($0.path, &tier) }
            .map { "\($0.lastPathComponent) \((try? manager.attributesOfItem(atPath: $0.path)[.size] as? Int) ?? 0)\n" }
            .sorted().joined()
        let record = variantsFolder.appendingPathComponent("source")
        guard !stamp.isEmpty, (try? String(contentsOf: record, encoding: .utf8)) != stamp else { return }
        for file in (try? manager.contentsOfDirectory(at: variantsFolder, includingPropertiesForKeys: nil)) ?? [] {
            try? manager.removeItem(at: file)
        }
        try? stamp.write(to: record, atomically: true, encoding: .utf8)
    }

    static func name(for tier: wp_tier) -> String? {
        var buffer = [CChar](repeating: 0, count: 128)
        guard wp_tier_name(WP_SCENE, tier, &buffer, buffer.count) > 0 else { return nil }
        return String(cString: buffer)
    }

    static func load(excluding broken: Set<URL> = []) -> Library {
        let manager = FileManager.default
        let bundled = Bundle.main.resourceURL.flatMap {
            try? manager.contentsOfDirectory(at: $0, includingPropertiesForKeys: nil)
        } ?? []
        forgetStale(made: bundled)
        let fitted = (try? manager.contentsOfDirectory(at: variantsFolder, includingPropertiesForKeys: nil)) ?? []
        let files = bundled + fitted
        var urls: [URL] = []
        var tiers: [wp_tier] = []
        for url in files.sorted(by: { $0.lastPathComponent < $1.lastPathComponent }) {
            var tier = wp_tier()
            guard !broken.contains(url), wp_tier_parse(url.path, &tier) else { continue }
            urls.append(url)
            tiers.append(tier)
        }
        return Library(urls: urls, tiers: tiers)
    }

    var summary: String {
        tiers.map { "\($0.width)×\($0.height)@\($0.fps)" }.joined(separator: ", ")
    }

    func pick(covering target: CGSize, rate: Double) -> Int? {
        let index = tiers.withUnsafeBufferPointer {
            wp_pick_tier($0.baseAddress, Int32($0.count), target.width, target.height, rate, Fit.current.core)
        }
        return index >= 0 ? Int(index) : nil
    }

    func isFitted(_ url: URL) -> Bool {
        url.deletingLastPathComponent().standardizedFileURL == Library.variantsFolder.standardizedFileURL
    }
}

enum Loop {
    static func asset(_ url: URL) async -> AVAsset {
        let asset = AVURLAsset(url: url)
        let length = MP4.duration(url)
        let repeats = wp_loop_repeats(length)
        guard repeats > 1 else { return asset }
        let one = CMTime(seconds: length, preferredTimescale: 1_000_000)
        let total = CMTime(seconds: length * Double(repeats), preferredTimescale: 1_000_000)
        let composition = AVMutableComposition()
        do {
            try await composition.insertTimeRange(CMTimeRange(start: .zero, duration: one), of: asset, at: .zero)
            var have = one
            while have < total {
                let span = CMTimeMinimum(have, total - have)
                try await composition.insertTimeRange(CMTimeRange(start: .zero, duration: span), of: composition, at: have)
                have = have + span
            }
            return composition
        } catch {
            return asset
        }
    }
}

enum MP4 {
    static func duration(_ url: URL) -> Double {
        guard let handle = try? FileHandle(forReadingFrom: url) else { return -1 }
        defer { try? handle.close() }
        let length = (try? handle.seekToEnd()) ?? 0
        let context = Unmanaged.passUnretained(handle).toOpaque()
        return wp_mp4_duration({ context, offset, buffer, size in
            guard let context, let buffer else { return 0 }
            let handle = Unmanaged<FileHandle>.fromOpaque(context).takeUnretainedValue()
            guard (try? handle.seek(toOffset: offset)) != nil,
                  let data = try? handle.read(upToCount: size), !data.isEmpty else { return 0 }
            data.copyBytes(to: buffer.assumingMemoryBound(to: UInt8.self), count: data.count)
            return data.count
        }, context, length)
    }
}

enum BoundedImage {
    static func load(_ url: URL, covering target: CGSize) -> CGImage? {
        guard let source = CGImageSourceCreateWithURL(url as CFURL, [kCGImageSourceShouldCache: false] as CFDictionary)
        else { return nil }
        var longest = max(target.width, target.height)
        var options: [CFString: Any] = [
            kCGImageSourceCreateThumbnailFromImageAlways: true,
            kCGImageSourceCreateThumbnailWithTransform: true,
            kCGImageSourceShouldCacheImmediately: true,
        ]
        if let properties = CGImageSourceCopyPropertiesAtIndex(source, 0, nil) as? [CFString: Any],
           let width = (properties[kCGImagePropertyPixelWidth] as? NSNumber)?.doubleValue,
           let height = (properties[kCGImagePropertyPixelHeight] as? NSNumber)?.doubleValue,
           width > 0, height > 0 {
            let fill = min(1.0, max(target.width / width, target.height / height))
            longest = ceil(max(width, height) * fill)
            var factor = 1
            while factor < 8, max(width, height) / Double(factor * 2) >= longest { factor *= 2 }
            if factor > 1 { options[kCGImageSourceSubsampleFactor] = factor }
        }
        options[kCGImageSourceThumbnailMaxPixelSize] = max(64, longest)
        return CGImageSourceCreateThumbnailAtIndex(source, 0, options as CFDictionary)
    }
}

enum Transcoder {
    struct Failure: LocalizedError {
        let errorDescription: String?
        init(_ message: String) { errorDescription = message }
    }

    static func destination(for source: URL, target: CGSize) -> URL {
        supportFolder()
            .appendingPathComponent("background-\(Int(target.width))x\(Int(target.height))-\(source.deletingPathExtension().lastPathComponent)")
            .appendingPathExtension("mp4")
    }

    static func convert(_ source: URL, to target: CGSize, progress: @escaping (Double) -> Void,
                        completion: @escaping (Result<URL, Error>) -> Void) {
        let output = destination(for: source, target: target)
        DispatchQueue.global(qos: .userInitiated).async {
            do {
                try run(source, target, output) { fraction in
                    DispatchQueue.main.async { progress(fraction) }
                }
                DispatchQueue.main.async { completion(.success(output)) }
            } catch {
                try? FileManager.default.removeItem(at: output)
                DispatchQueue.main.async { completion(.failure(error)) }
            }
        }
    }

    static func run(_ source: URL, _ target: CGSize, _ output: URL,
                    _ progress: (Double) -> Void) throws {
        let asset = AVURLAsset(url: source)
        let semaphore = DispatchSemaphore(value: 0)
        var loaded: (track: AVAssetTrack, size: CGSize, transform: CGAffineTransform, fps: Float, duration: CMTime)?
        var loadError: Error?
        Task {
            do {
                guard let track = try await asset.loadTracks(withMediaType: .video).first else {
                    throw Failure("The file has no video track.")
                }
                loaded = (track,
                          try await track.load(.naturalSize),
                          try await track.load(.preferredTransform),
                          try await track.load(.nominalFrameRate),
                          try await asset.load(.duration))
            } catch { loadError = error }
            semaphore.signal()
        }
        semaphore.wait()
        if let loadError { throw loadError }
        guard let info = loaded else { throw Failure("The file could not be read.") }
        let oriented = info.size.applying(info.transform)
        let shown = CGSize(width: abs(oriented.width), height: abs(oriented.height))
        guard shown.width >= 1, shown.height >= 1, target.width >= 16, target.height >= 16,
              shown.width.isFinite, shown.height.isFinite else {
            throw Failure("The video has no usable picture size.")
        }

        let fps = info.fps.isFinite && info.fps >= 1 ? min(120, Int(info.fps.rounded())) : 30
        let reader = try AVAssetReader(asset: asset)
        let readerOutput = AVAssetReaderTrackOutput(track: info.track, outputSettings: [
            kCVPixelBufferPixelFormatTypeKey as String: kCVPixelFormatType_420YpCbCr10BiPlanarVideoRange,
            kCVPixelBufferMetalCompatibilityKey as String: true,
            kCVPixelBufferIOSurfacePropertiesKey as String: [:],
        ])
        readerOutput.alwaysCopiesSampleData = false
        reader.add(readerOutput)

        try? FileManager.default.removeItem(at: output)
        let writer = try AVAssetWriter(outputURL: output, fileType: .mp4)
        let input = AVAssetWriterInput(mediaType: .video, outputSettings: [
            AVVideoCodecKey: AVVideoCodecType.hevc,
            AVVideoWidthKey: Int(target.width),
            AVVideoHeightKey: Int(target.height),
            AVVideoCompressionPropertiesKey: [
                AVVideoProfileLevelKey: kVTProfileLevel_HEVC_Main10_AutoLevel as String,
                AVVideoQualityKey: 0.65,
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
            kCVPixelBufferPixelFormatTypeKey as String: kCVPixelFormatType_ARGB2101010LEPacked,
            kCVPixelBufferWidthKey as String: Int(target.width),
            kCVPixelBufferHeightKey as String: Int(target.height),
            kCVPixelBufferMetalCompatibilityKey as String: true,
            kCVPixelBufferIOSurfacePropertiesKey as String: [:],
        ])
        writer.add(input)
        guard writer.startWriting() else { throw writer.error ?? Failure("The converted file could not be created.") }
        writer.startSession(atSourceTime: .zero)
        guard reader.startReading() else { throw reader.error ?? Failure("The file could not be read.") }

        let context = CIContext(options: [.workingColorSpace: NSNull(), .cacheIntermediates: false])
        let lanczos = CIFilter.lanczosScaleTransform()
        let crop = CGRect(origin: .zero, size: target)
        let scale = max(target.width / shown.width, target.height / shown.height)
        let seconds = info.duration.seconds
        let totalFrames = seconds.isFinite && seconds > 0 ? seconds * Double(fps) : 1
        var frames = 0

        while let sample = readerOutput.copyNextSampleBuffer() {
            try autoreleasepool {
                guard let pixels = CMSampleBufferGetImageBuffer(sample) else {
                    throw Failure("A frame could not be decoded.")
                }
                guard CMSampleBufferGetPresentationTimeStamp(sample).seconds * Double(fps) + 0.501 >= Double(frames) else { return }
                while !input.isReadyForMoreMediaData && writer.status == .writing { usleep(2000) }
                guard writer.status == .writing else { throw writer.error ?? Failure("Writing stopped.") }

                var image = CIImage(cvPixelBuffer: pixels)
                if !info.transform.isIdentity {
                    image = image.transformed(by: info.transform)
                    image = image.transformed(by: CGAffineTransform(translationX: -image.extent.minX, y: -image.extent.minY))
                }
                lanczos.inputImage = image.clampedToExtent()
                lanczos.scale = Float(scale)
                lanczos.aspectRatio = 1
                guard var scaled = lanczos.outputImage else { throw Failure("The frame could not be scaled.") }
                let dx = (shown.width * scale - target.width) / 2
                let dy = (shown.height * scale - target.height) / 2
                scaled = scaled.transformed(by: CGAffineTransform(translationX: -dx, y: -dy))
                var buffer: CVPixelBuffer?
                guard let pool = adaptor.pixelBufferPool,
                      CVPixelBufferPoolCreatePixelBuffer(nil, pool, &buffer) == kCVReturnSuccess,
                      let destination = buffer else { throw Failure("Out of memory for the frame.") }
                context.render(scaled.cropped(to: crop), to: destination, bounds: crop, colorSpace: nil)
                guard adaptor.append(destination, withPresentationTime: CMTime(value: Int64(frames), timescale: Int32(fps)))
                else { throw writer.error ?? Failure("A frame could not be written.") }
                frames += 1
                if frames % 15 == 0 { progress(min(0.99, Double(frames) / totalFrames)) }
            }
        }
        guard reader.status == .completed else { throw reader.error ?? Failure("Reading stopped.") }
        input.markAsFinished()
        let finished = DispatchSemaphore(value: 0)
        writer.finishWriting { finished.signal() }
        finished.wait()
        guard writer.status == .completed else { throw writer.error ?? Failure("The conversion did not finish.") }
        guard frames > 0 else { throw Failure("The file contained no frames.") }
        progress(1)
    }
}

final class StillMaker {
    private var generators: [URL: AVAssetImageGenerator] = [:]

    func image(for url: URL, at time: CMTime, maxSize: CGSize, completion: @escaping (CGImage?) -> Void) {
        let generator: AVAssetImageGenerator
        if let existing = generators[url] {
            generator = existing
        } else {
            generator = AVAssetImageGenerator(asset: AVURLAsset(url: url))
            generator.appliesPreferredTrackTransform = true
            generator.requestedTimeToleranceBefore = CMTime(value: 1, timescale: 15)
            generator.requestedTimeToleranceAfter = CMTime(value: 1, timescale: 15)
            generators[url] = generator
        }
        generator.maximumSize = maxSize
        generator.generateCGImageAsynchronously(for: time) { image, _, _ in
            DispatchQueue.main.async { completion(image) }
        }
    }

    func forget(_ url: URL) { generators[url] = nil }
}

final class PowerMonitor {
    private(set) var onBattery = false
    private(set) var lowPower = false
    private(set) var hot = false
    var onChange: (() -> Void)?
    private var source: CFRunLoopSource?

    init() {
        refreshSource()
        refreshProcessInfo()

        let context = Unmanaged.passUnretained(self).toOpaque()
        let callback: IOPowerSourceCallbackType = { pointer in
            guard let pointer else { return }
            let monitor = Unmanaged<PowerMonitor>.fromOpaque(pointer).takeUnretainedValue()
            monitor.refreshSource()
            monitor.onChange?()
        }
        if let runLoopSource = IOPSNotificationCreateRunLoopSource(callback, context)?.takeRetainedValue() {
            CFRunLoopAddSource(CFRunLoopGetMain(), runLoopSource, .defaultMode)
            source = runLoopSource
        }
        for name in [NSNotification.Name.NSProcessInfoPowerStateDidChange, ProcessInfo.thermalStateDidChangeNotification] {
            NotificationCenter.default.addObserver(forName: name, object: nil, queue: .main) { [weak self] _ in
                self?.refreshProcessInfo()
                self?.onChange?()
            }
        }
    }

    private func refreshProcessInfo() {
        lowPower = ProcessInfo.processInfo.isLowPowerModeEnabled
        let state = ProcessInfo.processInfo.thermalState
        hot = state == .serious || state == .critical
    }

    private func refreshSource() {
        guard let blob = IOPSCopyPowerSourcesInfo()?.takeRetainedValue(),
              let type = IOPSGetProvidingPowerSourceType(blob)?.takeRetainedValue() as String? else { return }
        onBattery = type == "Battery Power"
    }
}

enum Coverage {
    static func isCovered(displayID: CGDirectDisplayID, screen: NSScreen, ownPID: pid_t) -> Bool {
        let frame = screen.frame, visible = screen.visibleFrame
        var region = CGDisplayBounds(displayID)
        let top = frame.maxY - visible.maxY, bottom = visible.minY - frame.minY
        let left = visible.minX - frame.minX, right = frame.maxX - visible.maxX
        region.origin.x += left
        region.size.width -= left + right
        region.origin.y += top
        region.size.height -= top + bottom
        guard let list = CGWindowListCopyWindowInfo([.optionOnScreenOnly], kCGNullWindowID) as? [[String: Any]] else {
            return false
        }
        var blockers: [wp_rect] = []
        blockers.reserveCapacity(list.count)
        for window in list {
            guard (window[kCGWindowLayer as String] as? Int) == 0,
                  (window[kCGWindowAlpha as String] as? Double ?? 0) >= 0.95,
                  (window[kCGWindowIsOnscreen as String] as? Bool) == true,
                  (window[kCGWindowOwnerPID as String] as? pid_t) != ownPID,
                  let bounds = window[kCGWindowBounds as String] as? NSDictionary,
                  let rect = CGRect(dictionaryRepresentation: bounds as CFDictionary),
                  rect.intersects(region)
            else { continue }
            blockers.append(wp_rect(rect))
        }
        return blockers.withUnsafeBufferPointer {
            wp_covered(wp_rect(region), $0.baseAddress, Int32($0.count))
        }
    }
}

final class DesktopWindow: NSWindow {
    override var canBecomeKey: Bool { false }
    override var canBecomeMain: Bool { false }
}

final class WallpaperView: NSView {
    let still = CALayer()
    private(set) var playerLayer: AVPlayerLayer?
    private var previous: AVPlayerLayer?
    var canvas: CGRect = .zero { didSet { needsLayout = true } }

    override init(frame frameRect: NSRect) {
        super.init(frame: frameRect)
        wantsLayer = true
        layerContentsRedrawPolicy = .never
        guard let root = layer else { return }
        root.isOpaque = true
        root.backgroundColor = NSColor.black.cgColor
        root.masksToBounds = true
        still.contentsGravity = Fit.current.layerGravity
        still.contentsScale = window?.backingScaleFactor ?? 2
        still.isOpaque = true
        still.masksToBounds = true
        still.actions = ["contents": NSNull(), "bounds": NSNull(), "position": NSNull()]
        root.addSublayer(still)
    }

    required init?(coder: NSCoder) { nil }

    override var isOpaque: Bool { true }

    override func viewDidMoveToWindow() {
        super.viewDidMoveToWindow()
        let scale = window?.backingScaleFactor ?? 2
        still.contentsScale = scale
        playerLayer?.contentsScale = scale
    }

    func applyFit() {
        CATransaction.begin()
        CATransaction.setDisableActions(true)
        still.contentsGravity = Fit.current.layerGravity
        playerLayer?.videoGravity = Fit.current.videoGravity
        CATransaction.commit()
    }

    func attach(_ layer: AVPlayerLayer, keepingPrevious: Bool = false) {
        if keepingPrevious {
            dropPrevious()
            previous = playerLayer
        } else {
            detach()
        }
        layer.videoGravity = Fit.current.videoGravity
        layer.isOpaque = true
        layer.contentsScale = window?.backingScaleFactor ?? 2
        layer.frame = canvas == .zero ? bounds : canvas
        layer.actions = ["bounds": NSNull(), "position": NSNull(), "contents": NSNull()]
        self.layer?.addSublayer(layer)
        playerLayer = layer
    }

    func detach() {
        playerLayer?.removeFromSuperlayer()
        playerLayer = nil
        dropPrevious()
    }

    func setHidden(_ layer: CALayer, _ hidden: Bool) {
        CATransaction.begin()
        CATransaction.setDisableActions(true)
        layer.isHidden = hidden
        CATransaction.commit()
    }

    func dropPrevious() {
        previous?.removeFromSuperlayer()
        previous = nil
    }

    func show(_ image: CGImage?) {
        CATransaction.begin()
        CATransaction.setDisableActions(true)
        still.contents = image
        CATransaction.commit()
    }

    override func layout() {
        super.layout()
        CATransaction.begin()
        CATransaction.setDisableActions(true)
        let area = canvas == .zero ? bounds : canvas
        still.frame = area
        playerLayer?.frame = area
        previous?.frame = area
        CATransaction.commit()
    }
}

final class Surface {
    let displayID: CGDirectDisplayID
    let window: DesktopWindow
    let view: WallpaperView
    var covered = false

    init(screen: NSScreen, displayID: CGDirectDisplayID, canvas: CGRect) {
        self.displayID = displayID
        window = DesktopWindow(contentRect: screen.frame, styleMask: .borderless, backing: .buffered, defer: false, screen: screen)
        window.level = NSWindow.Level(rawValue: Int(CGWindowLevelForKey(.desktopWindow)) + 1)
        window.collectionBehavior = [.canJoinAllSpaces, .stationary, .ignoresCycle]
        window.isOpaque = true
        window.backgroundColor = .black
        window.hasShadow = false
        window.ignoresMouseEvents = true
        window.isReleasedWhenClosed = false
        window.isRestorable = false
        window.animationBehavior = .none
        window.tabbingMode = .disallowed
        view = WallpaperView(frame: NSRect(origin: .zero, size: screen.frame.size))
        window.contentView = view
        window.setFrame(screen.frame, display: false)
        view.canvas = canvas.offsetBy(dx: -screen.frame.minX, dy: -screen.frame.minY)
        window.orderFrontRegardless()
    }

    func update(screen: NSScreen, canvas: CGRect) {
        if window.frame != screen.frame {
            window.setFrame(screen.frame, display: false)
            view.frame = NSRect(origin: .zero, size: screen.frame.size)
        }
        let local = canvas.offsetBy(dx: -screen.frame.minX, dy: -screen.frame.minY)
        if view.canvas != local { view.canvas = local }
        view.applyFit()
        view.needsLayout = true
    }

    func dispose() {
        view.detach()
        view.show(nil)
        window.orderOut(nil)
    }
}

final class Playback {
    private(set) var background: Background?
    private var core = wp_surface()
    private var choice = wp_choice()
    private var surfaces: [CGDirectDisplayID: Surface] = [:]

    private var player: AVQueuePlayer?
    private var looper: AVPlayerLooper?
    private var layers: [CGDirectDisplayID: AVPlayerLayer] = [:]
    private var readyObservers: [CGDirectDisplayID: NSKeyValueObservation] = [:]
    private var itemObserver: NSKeyValueObservation?
    private var retiring: (AVQueuePlayer, AVPlayerLooper?)?
    private var pendingSeek: (time: Double, at: Double, rate: Double)?
    private var seeking = false
    private var settled: Set<CGDirectDisplayID> = []
    private var generation = 0
    private var length = 0.0
    private var looperObserver: NSKeyValueObservation?
    private var loop: (url: URL, asset: AVAsset)?
    var onFailure: ((Background) -> Void)?

    private let still: StillMaker
    private let stillSize: () -> CGSize
    private var stillImage: CGImage?
    private var showingStill = false
    private var stillPending = false

    init(still: StillMaker, stillSize: @escaping () -> CGSize) {
        self.still = still
        self.stillSize = stillSize
        wp_surface_init(&core)
        wp_choice_init(&choice)
    }

    var isVideo: Bool { background?.isVideo ?? false }
    var state: wp_state { core.state }
    var releaseDeadline: Double { wp_surface_deadline(&core, WP_RELEASE_DELAY) }
    var switchDeadline: Double { wp_choice_deadline(&choice, WP_SWITCH_HOLD) }

    func attach(_ current: [Surface]) {
        let ids = Set(current.map(\.displayID))
        for id in Array(surfaces.keys) where !ids.contains(id) {
            readyObservers[id]?.invalidate()
            readyObservers[id] = nil
            layers[id] = nil
            settled.remove(id)
            surfaces[id] = nil
        }
        for surface in current where surfaces[surface.displayID] == nil {
            surfaces[surface.displayID] = surface
            if showingStill { surface.view.show(stillImage) }
            if let player { addLayer(for: surface, player: player, keepingPrevious: false) }
        }
        if !current.isEmpty && settled.isSuperset(of: layers.keys) { finishRetiring() }
    }

    func offer(_ new: Background, immediate: Bool, sameLoop: Bool) {
        guard wp_choice_offer(&choice, wp_hash(new.url.path), uptime(), WP_SWITCH_HOLD, immediate || background == nil),
              new != background else { return }
        switchTo(new, keepTime: sameLoop)
    }

    private var position: Double {
        let time = player?.currentTime().seconds ?? 0
        guard time.isFinite, time > 0 else { return 0 }
        return length > 0 ? fmod(time, length) : time
    }

    func clear() {
        teardown()
        loop = nil
        background = nil
        length = 0
        stillImage = nil
        stillPending = false
        pendingSeek = nil
        showStill(nil)
        wp_surface_init(&core)
        wp_choice_init(&choice)
    }

    private func failed() {
        guard let broken = background else { return }
        log("cannot play \(broken.url.lastPathComponent)")
        clear()
        onFailure?(broken)
    }

    func refresh() {
        guard let background, showingStill || !background.isVideo else { return }
        stillPending = false
        let held = player != nil ? position : max(0, core.resume)
        loadStill(at: background.isVideo ? CMTime(seconds: held, preferredTimescale: 600) : .zero)
    }

    private func switchTo(_ new: Background, keepTime: Bool) {
        let time = position
        log("content -> \(new.url.lastPathComponent) from t=\(String(format: "%.3f", time))")
        let playing = core.state == WP_PLAYING
        let first = background == nil
        background = new
        length = new.isVideo ? MP4.duration(new.url) : 0
        if keepTime, playing, new.isVideo, player != nil {
            pendingSeek = (time, uptime(), core.rate)
            retire()
            build(keepingPrevious: true)
            return
        }
        let held = player != nil ? time : core.resume
        wp_surface_init(&core)
        pendingSeek = nil
        stillPending = false
        if keepTime, new.isVideo, held > 0, !first {
            wp_surface_hold(&core, held)
            let mark = generation
            loadStill(at: CMTime(seconds: held, preferredTimescale: 600)) { [weak self] in
                guard let self, self.generation == mark else { return }
                self.teardown()
            }
            return
        }
        teardown()
        stillImage = nil
        showStill(nil)
        loadStill(at: .zero)
    }

    func step(_ want: wp_want, rate: Double, now: Double) {
        guard isVideo else { return }
        let before = core.state
        let action = wp_surface_step(&core, want, rate, now, WP_RELEASE_DELAY)
        if core.state != before {
            log("playback: \(before.rawValue) -> \(core.state.rawValue) rate \(rate)")
        }
        if action.build {
            if action.start > 0 { pendingSeek = (action.start, now, 0) }
            build(keepingPrevious: false)
        }
        if action.play { player?.rate = Float(action.rate) }
        if action.pause { player?.pause() }
        if action.release {
            if player != nil { wp_surface_hold(&core, position) }
            freeze()
        }
    }

    private func freeze() {
        let time = CMTime(seconds: position, preferredTimescale: 600)
        guard !showingStill else { return teardown() }
        let mark = generation
        stillPending = false
        loadStill(at: time) { [weak self] in
            guard let self, self.generation == mark, self.core.state == WP_RELEASED else { return }
            self.teardown()
        }
    }

    private func showStill(_ image: CGImage?) {
        showingStill = image != nil
        for surface in surfaces.values { surface.view.show(image) }
    }

    private func loadStill(at time: CMTime, then finish: (() -> Void)? = nil) {
        guard !stillPending, let content = background else { return }
        stillPending = true
        let target = stillSize()
        switch content {
        case .image(let url):
            DispatchQueue.global(qos: .userInitiated).async {
                let image = BoundedImage.load(url, covering: target)
                DispatchQueue.main.async { [weak self] in
                    guard let self, self.background == content else { return }
                    self.stillPending = false
                    guard let image else { return self.failed() }
                    self.stillImage = image
                    self.showStill(image)
                    finish?()
                }
            }
        case .video(let url):
            still.image(for: url, at: time, maxSize: target) { [weak self] image in
                guard let self, self.background == content else { return }
                self.stillPending = false
                if let image {
                    self.stillImage = image
                    self.showStill(image)
                }
                finish?()
            }
        }
    }

    private func build(keepingPrevious: Bool) {
        guard let background else { return }
        if player != nil { teardown() }
        generation += 1
        if let loop, loop.url == background.url {
            return start(AVPlayerItem(asset: loop.asset), keepingPrevious: keepingPrevious)
        }
        let mark = generation
        Task { @MainActor [weak self] in
            let asset = await Loop.asset(background.url)
            guard let self, self.generation == mark, self.background == background else { return }
            self.loop = (background.url, asset)
            self.start(AVPlayerItem(asset: asset), keepingPrevious: keepingPrevious)
        }
    }

    private func start(_ item: AVPlayerItem, keepingPrevious: Bool) {
        item.preferredForwardBufferDuration = 2
        let queue = AVQueuePlayer()
        queue.isMuted = true
        queue.volume = 0
        queue.automaticallyWaitsToMinimizeStalling = false
        queue.preventsDisplaySleepDuringVideoPlayback = false
        queue.actionAtItemEnd = .advance
        let looper = AVPlayerLooper(player: queue, templateItem: item)
        self.looper = looper
        looperObserver = looper.observe(\.status, options: [.initial, .new]) { [weak self] looper, _ in
            guard looper.status == .failed else { return }
            DispatchQueue.main.async { self?.failed() }
        }
        player = queue
        seeking = false
        settled = []
        for surface in surfaces.values {
            addLayer(for: surface, player: queue, keepingPrevious: keepingPrevious)
        }
        itemObserver = queue.observe(\.currentItem) { [weak self] queue, _ in
            guard let self, self.core.state == WP_PLAYING else { return }
            let rate = Float(self.core.rate)
            if queue.rate != rate { queue.rate = rate }
        }
        if core.state == WP_PLAYING { queue.rate = Float(core.rate) }
    }

    private func addLayer(for surface: Surface, player queue: AVQueuePlayer, keepingPrevious: Bool) {
        let id = surface.displayID
        let layer = AVPlayerLayer(player: queue)
        surface.view.attach(layer, keepingPrevious: keepingPrevious)
        layers[id] = layer
        settled.remove(id)
        if pendingSeek != nil || seeking { surface.view.setHidden(layer, true) }
        readyObservers[id] = layer.observe(\.isReadyForDisplay, options: [.initial, .new]) { [weak self] layer, _ in
            guard layer.isReadyForDisplay, let self, self.layers[id] === layer else { return }
            self.layerReady(id)
        }
    }

    private func layerReady(_ id: CGDirectDisplayID) {
        guard let queue = player else { return }
        if let seek = pendingSeek {
            pendingSeek = nil
            seeking = true
            var target = seek.time + (uptime() - seek.at) * seek.rate
            if length > 0 { target = fmod(target, length) }
            if !target.isFinite || target < 0 { target = 0 }
            let tolerance = CMTime(value: 1, timescale: 60)
            let mark = generation
            queue.seek(to: CMTime(seconds: target, preferredTimescale: 600),
                       toleranceBefore: tolerance, toleranceAfter: tolerance) { [weak self] _ in
                DispatchQueue.main.async {
                    guard let self, self.generation == mark else { return }
                    log("resumed at t=\(String(format: "%.3f", queue.currentTime().seconds)), target \(String(format: "%.3f", target))")
                    self.seeking = false
                    for (id, layer) in self.layers where layer.isReadyForDisplay { self.settle(id) }
                }
            }
            return
        }
        guard !seeking else { return }
        settle(id)
    }

    private func settle(_ id: CGDirectDisplayID) {
        guard let layer = layers[id], let surface = surfaces[id], !settled.contains(id) else { return }
        settled.insert(id)
        surface.view.setHidden(layer, false)
        surface.view.show(nil)
        surface.view.dropPrevious()
        if settled.isSuperset(of: layers.keys) {
            showingStill = false
            finishRetiring()
        }
    }

    private func retire() {
        looperObserver?.invalidate(); looperObserver = nil
        readyObservers.values.forEach { $0.invalidate() }
        readyObservers = [:]
        itemObserver?.invalidate(); itemObserver = nil
        finishRetiring()
        if let player { retiring = (player, looper) }
        player = nil
        looper = nil
        layers = [:]
        settled = []
    }

    private func finishRetiring() {
        guard let (old, oldLooper) = retiring else { return }
        oldLooper?.disableLooping()
        old.pause()
        old.removeAllItems()
        retiring = nil
    }

    func teardown() {
        generation += 1
        looperObserver?.invalidate(); looperObserver = nil
        readyObservers.values.forEach { $0.invalidate() }
        readyObservers = [:]
        itemObserver?.invalidate(); itemObserver = nil
        looper?.disableLooping(); looper = nil
        player?.pause()
        player?.removeAllItems()
        for layer in layers.values { layer.player = nil }
        for surface in surfaces.values { surface.view.detach() }
        layers = [:]
        settled = []
        seeking = false
        player = nil
        finishRetiring()
    }
}

enum WallpaperWriter {
    static func canvas() -> CGRect {
        let frames = NSScreen.screens.map { wp_rect($0.frame) }
        guard !frames.isEmpty else { return .null }
        return CGRect(frames.withUnsafeBufferPointer { wp_canvas($0.baseAddress, Int32($0.count)) })
    }

    static func backing(_ area: CGRect) -> CGSize {
        let scale = NSScreen.screens.map(\.backingScaleFactor).max() ?? 2
        return CGSize(width: area.width * scale, height: area.height * scale)
    }

    static func run(_ url: URL) -> Bool {
        let area = canvas()
        guard !area.isNull, area.width > 1 else { return false }
        let target = backing(area)
        let type = url.pathExtension.lowercased()
        var frame: CGImage?
        if Media.imageTypes.contains(type) {
            frame = BoundedImage.load(url, covering: target)
        } else {
            let generator = AVAssetImageGenerator(asset: AVURLAsset(url: url))
            generator.appliesPreferredTrackTransform = true
            generator.requestedTimeToleranceBefore = CMTime(value: 1, timescale: 15)
            generator.requestedTimeToleranceAfter = CMTime(value: 1, timescale: 15)
            generator.maximumSize = target
            let done = DispatchSemaphore(value: 0)
            generator.generateCGImageAsynchronously(for: .zero) { image, _, _ in
                frame = image
                done.signal()
            }
            _ = done.wait(timeout: .now() + 30)
        }
        guard let image = frame else { return false }
        return write(image, area, NSScreen.screens)
    }

    static func displayedRect(_ image: CGSize, in area: CGRect, fit: Fit) -> CGRect {
        CGRect(wp_displayed(image.width, image.height, wp_rect(area), fit.core))
    }

    @discardableResult
    static func write(_ image: CGImage, _ area: CGRect, _ screens: [NSScreen]) -> Bool {
        let folder = supportFolder()
        let stamp = Int(Date().timeIntervalSince1970)
        let fit = Fit.current
        let source = CGSize(width: image.width, height: image.height)
        var applied: Set<String> = []
        for (index, screen) in screens.enumerated() {
            let scale = screen.backingScaleFactor
            let width = Int(screen.frame.width * scale), height = Int(screen.frame.height * scale)
            guard width > 0, height > 0,
                  let context = CGContext(data: nil, width: width, height: height, bitsPerComponent: 8,
                                          bytesPerRow: 0, space: CGColorSpace(name: CGColorSpace.sRGB) ?? CGColorSpaceCreateDeviceRGB(),
                                          bitmapInfo: CGImageAlphaInfo.noneSkipFirst.rawValue)
            else { continue }
            context.setFillColor(CGColor(gray: 0, alpha: 1))
            context.fill(CGRect(x: 0, y: 0, width: width, height: height))
            context.interpolationQuality = .high
            let shown = displayedRect(source, in: area, fit: fit)
            context.draw(image, in: CGRect(x: (shown.minX - screen.frame.minX) * scale,
                                           y: (shown.minY - screen.frame.minY) * scale,
                                           width: shown.width * scale, height: shown.height * scale))
            guard let output = context.makeImage() else { continue }
            let url = folder.appendingPathComponent("wallpaper-\(index)-\(stamp).jpg")
            guard let destination = CGImageDestinationCreateWithURL(
                url as CFURL, UTType.jpeg.identifier as CFString, 1, nil) else { continue }
            CGImageDestinationAddImage(destination, output,
                                       [kCGImageDestinationLossyCompressionQuality: 0.9] as CFDictionary)
            guard CGImageDestinationFinalize(destination) else { continue }
            do {
                try NSWorkspace.shared.setDesktopImageURL(url, for: screen, options: [:])
                applied.insert(url.lastPathComponent)
            } catch {
                FileHandle.standardError.write(Data("wallpaper: \(error.localizedDescription)\n".utf8))
                try? FileManager.default.removeItem(at: url)
            }
        }
        var keep = applied
        for screen in screens {
            if let current = NSWorkspace.shared.desktopImageURL(for: screen) {
                keep.insert(current.lastPathComponent)
            }
        }
        for file in (try? FileManager.default.contentsOfDirectory(at: folder, includingPropertiesForKeys: nil)) ?? [] {
            let name = file.lastPathComponent
            if name.hasPrefix("wallpaper-"), !keep.contains(name) {
                try? FileManager.default.removeItem(at: file)
            }
        }
        return !applied.isEmpty
    }
}

final class SpeedRow: NSView, NSTextFieldDelegate {
    private let slider = NSSlider(value: 0.5, minValue: 0, maxValue: 1, target: nil, action: nil)
    private let field = NSTextField()
    private let unit = NSTextField(labelWithString: "×")
    var onChange: ((Double) -> Void)?

    init() {
        super.init(frame: NSRect(x: 0, y: 0, width: 240, height: 32))
        autoresizingMask = .width
        slider.numberOfTickMarks = 5
        slider.isContinuous = true
        slider.target = self
        slider.action = #selector(slid)
        slider.frame = NSRect(x: 20, y: 4, width: 140, height: 24)
        slider.autoresizingMask = .width
        field.font = .monospacedDigitSystemFont(ofSize: NSFont.systemFontSize, weight: .regular)
        field.alignment = .right
        field.delegate = self
        field.target = self
        field.action = #selector(typed)
        field.frame = NSRect(x: 168, y: 5, width: 50, height: 22)
        field.autoresizingMask = .minXMargin
        unit.font = .menuFont(ofSize: 0)
        unit.textColor = .secondaryLabelColor
        unit.frame = NSRect(x: 220, y: 8, width: 14, height: 16)
        unit.autoresizingMask = .minXMargin
        [slider, field, unit].forEach(addSubview)
    }

    required init?(coder: NSCoder) { nil }

    var speed = 1.0 { didSet { show() } }

    private func show() {
        slider.doubleValue = wp_slider_position(speed)
        field.stringValue = String(format: "%g", speed)
    }

    @objc private func slid() {
        speed = wp_slider_speed(slider.doubleValue)
        onChange?(speed)
    }

    @objc private func typed() {
        var value = 0.0
        guard wp_speed_parse(field.stringValue, &value) else { return show() }
        speed = wp_speed_clamp(value)
        onChange?(speed)
    }

    func control(_ control: NSControl, textView: NSTextView, doCommandBy selector: Selector) -> Bool {
        switch selector {
        case #selector(NSResponder.insertNewline(_:)):
            typed()
        case #selector(NSResponder.cancelOperation(_:)):
            field.abortEditing()
            show()
        default:
            return false
        }
        enclosingMenuItem?.menu?.cancelTracking()
        return true
    }
}

final class Engine: NSObject, NSApplicationDelegate, NSMenuDelegate, NSMenuItemValidation {
    private var surfaces: [CGDirectDisplayID: Surface] = [:]
    private var library = Library.load()
    private let still = StillMaker()
    private lazy var playback = Playback(still: still) { [unowned self] in self.canvasBacking }
    private var lastBacking = CGSize.zero
    private var broken: Set<URL> = []
    private var wallpaperAttempts = (signature: "", count: 0)
    private var fitting: (process: Process, name: String)?
    private var pendingFit: (work: DispatchWorkItem, name: String)?
    private var refusedFits: Set<String> = []
    private var touchedFit: URL?
    private var fitAllowed = false

    private let power = PowerMonitor()
    private var status: NSStatusItem?
    private var stateItem: NSMenuItem?
    private var pauseItem: NSMenuItem?
    private var contentItem: NSMenuItem?
    private var loginItem: NSMenuItem?
    private var fitItems: [NSMenuItem] = []
    private var prefItems: [NSMenuItem] = []
    private let speedRow = SpeedRow()

    private var manualPause = false
    private var displayAsleep = false
    private var screensaver = false
    private var locked = false
    private var sessionInactive = false
    private var converting = false

    private var wakeTimer: Timer?
    private var lastPlan = ""
    private var lastRate = -1.0
    private var mouseMonitor: Any?
    private var reconcileWork: DispatchWorkItem?
    private var lastWallpaperSignature = ""
    private var coalesced: DispatchWorkItem?
    private var followUp: DispatchWorkItem?
    private var lastCheck = Date.distantPast
    private var lastFrozen: Bool?
    private let ownPID = ProcessInfo.processInfo.processIdentifier

    func applicationDidFinishLaunching(_ notification: Notification) {
        Pref.register()
        retirePreviousInstances()
        Login.refresh()
        try? FileManager.default.removeItem(at: Library.variantsFolder.appendingPathComponent("tmp", isDirectory: true))

        playback.onFailure = { [weak self] content in self?.contentFailed(content) }
        buildMenu()
        reconcile()

        NotificationCenter.default.addObserver(self, selector: #selector(screensChanged),
                                               name: NSApplication.didChangeScreenParametersNotification, object: nil)

        let workspace = NSWorkspace.shared.notificationCenter
        workspace.addObserver(self, selector: #selector(displaySleep), name: NSWorkspace.screensDidSleepNotification, object: nil)
        workspace.addObserver(self, selector: #selector(displayWake), name: NSWorkspace.screensDidWakeNotification, object: nil)
        workspace.addObserver(self, selector: #selector(sessionOut), name: NSWorkspace.sessionDidResignActiveNotification, object: nil)
        workspace.addObserver(self, selector: #selector(sessionIn), name: NSWorkspace.sessionDidBecomeActiveNotification, object: nil)
        for name in [NSWorkspace.didActivateApplicationNotification,
                     NSWorkspace.activeSpaceDidChangeNotification,
                     NSWorkspace.didLaunchApplicationNotification,
                     NSWorkspace.didTerminateApplicationNotification,
                     NSWorkspace.didUnhideApplicationNotification,
                     NSWorkspace.didHideApplicationNotification] {
            workspace.addObserver(self, selector: #selector(layoutMayHaveChanged), name: name, object: nil)
        }

        let distributed = DistributedNotificationCenter.default()
        distributed.addObserver(self, selector: #selector(screensaverStart), name: .init("com.apple.screensaver.didstart"), object: nil)
        distributed.addObserver(self, selector: #selector(screensaverStop), name: .init("com.apple.screensaver.didstop"), object: nil)
        distributed.addObserver(self, selector: #selector(screenLocked), name: .init("com.apple.screenIsLocked"), object: nil)
        distributed.addObserver(self, selector: #selector(screenUnlocked), name: .init("com.apple.screenIsUnlocked"), object: nil)

        power.onChange = { [weak self] in self?.updatePlayback() }

        mouseMonitor = NSEvent.addGlobalMonitorForEvents(matching: [.leftMouseUp, .otherMouseUp]) { [weak self] _ in
            self?.layoutMayHaveChanged()
        }
        log("launch: variants \(library.summary), mouse monitor \(mouseMonitor != nil ? "on" : "unavailable")")
        updatePlayback()
    }

    private func retirePreviousInstances() {
        let mine = Bundle.main.bundleIdentifier ?? "local.eventhorizon.wallpaper"
        for app in NSRunningApplication.runningApplications(withBundleIdentifier: mine)
        where app.processIdentifier != ownPID {
            app.terminate()
        }
        for app in NSWorkspace.shared.runningApplications {
            guard let id = app.bundleIdentifier, id != mine,
                  id.hasPrefix("local.eventhorizon"), app.processIdentifier != ownPID else { continue }
            app.terminate()
        }
    }

    @objc private func screensChanged() {
        log("screen parameters changed")
        reconcileWork?.cancel()
        let work = DispatchWorkItem { [weak self] in self?.reconcile() }
        reconcileWork = work
        DispatchQueue.main.asyncAfter(deadline: .now() + 0.4, execute: work)
    }

    private var canvas: CGRect { WallpaperWriter.canvas() }

    private var canvasBacking: CGSize {
        let scale = NSScreen.screens.map(\.backingScaleFactor).max() ?? 2
        let area = canvas
        guard !area.isNull else { return CGSize(width: 1920, height: 1200) }
        return CGSize(width: area.width * scale, height: area.height * scale)
    }

    private func background(for target: CGSize, rate: Double) -> Background? {
        let path = Pref.text(Pref.contentPath)
        let kind = Pref.text(Pref.contentKind)
        if !path.isEmpty, FileManager.default.fileExists(atPath: path) {
            let url = URL(fileURLWithPath: path)
            return kind == "image" ? .image(url) : .video(url)
        }
        guard let index = library.pick(covering: target, rate: rate) else { return nil }
        let url = library.urls[index]
        if library.isFitted(url), touchedFit != url {
            touchedFit = url
            try? FileManager.default.setAttributes([.modificationDate: Date()], ofItemAtPath: url.path)
        }
        return .video(url)
    }

    private var builtIn: Bool {
        let path = Pref.text(Pref.contentPath)
        return path.isEmpty || !FileManager.default.fileExists(atPath: path)
    }

    private func reconcile(immediate: Bool = false) {
        let area = canvas
        guard !area.isNull else { return }
        let rate = self.rate
        lastRate = rate
        guard let content = background(for: canvasBacking, rate: rate) else { return clearSurfaces() }
        let sameLoop = builtIn && playback.background.map { library.urls.contains($0.url) } == true
        var live = Set<CGDirectDisplayID>()
        for screen in NSScreen.screens {
            guard let number = screen.deviceDescription[NSDeviceDescriptionKey("NSScreenNumber")] as? NSNumber else { continue }
            let id = CGDirectDisplayID(number.uint32Value)
            live.insert(id)
            if let surface = surfaces[id] {
                surface.update(screen: screen, canvas: area)
            } else {
                log("reconcile screen \(id): canvas=\(area.size) -> \(content.url.lastPathComponent)")
                surfaces[id] = Surface(screen: screen, displayID: id, canvas: area)
            }
        }
        for (id, surface) in surfaces where !live.contains(id) {
            surface.dispose()
            surfaces[id] = nil
        }
        playback.attach(Array(surfaces.values))
        playback.offer(content, immediate: immediate, sameLoop: sameLoop)
        let backing = canvasBacking
        if lastBacking != .zero, backing != lastBacking { playback.refresh() }
        lastBacking = backing
        considerFitted(content)
        updatePlayback()
        syncSystemWallpaper()
    }

    private func clearSurfaces() {
        pendingFit?.work.cancel()
        pendingFit = nil
        playback.clear()
        surfaces.values.forEach { $0.dispose() }
        surfaces = [:]
        playback.attach([])
        log("nothing to show: no readable background")
    }

    private func contentFailed(_ content: Background) {
        if library.urls.contains(content.url) {
            broken.insert(content.url)
            if library.isFitted(content.url) { try? FileManager.default.removeItem(at: content.url) }
            library = Library.load(excluding: broken)
            reconcile(immediate: true)
            return
        }
        Pref.set(Pref.contentPath, "")
        Pref.set(Pref.contentKind, "")
        reconcile(immediate: true)
        NSApp.activate(ignoringOtherApps: true)
        let alert = NSAlert()
        alert.messageText = "That background cannot be shown."
        alert.informativeText = library.tiers.isEmpty
            ? "\(content.url.lastPathComponent) could not be decoded."
            : "\(content.url.lastPathComponent) could not be decoded, so the built-in background is back."
        alert.runModal()
    }

    private func considerFitted(_ content: Background) {
        var state = system
        var tier = wp_tier()
        let target = canvasBacking
        guard builtIn, case .video(let url) = content, let index = library.urls.firstIndex(of: url),
              wp_fitted_allowed(&state),
              wp_fitted_tier(library.tiers[index], target.width, target.height, Fit.current.core, &tier),
              let name = Library.name(for: tier) else {
            pendingFit?.work.cancel()
            pendingFit = nil
            return
        }
        let destination = Library.variantsFolder.appendingPathComponent(name)
        guard fitting == nil, pendingFit?.name != name, !refusedFits.contains(name),
              !FileManager.default.fileExists(atPath: destination.path) else { return }
        pendingFit?.work.cancel()
        let work = DispatchWorkItem { [weak self] in
            self?.pendingFit = nil
            self?.startFitted(source: url, tier: tier, name: name)
        }
        pendingFit = (work, name)
        log("fitted variant \(name) planned")
        DispatchQueue.main.asyncAfter(deadline: .now() + WP_FIT_DELAY, execute: work)
    }

    private func startFitted(source: URL, tier: wp_tier, name: String) {
        var state = system
        guard fitting == nil, wp_fitted_allowed(&state), let executable = Bundle.main.executableURL else { return }
        let temporary = Library.variantsFolder.appendingPathComponent("tmp", isDirectory: true)
        try? FileManager.default.createDirectory(at: temporary, withIntermediateDirectories: true)
        let output = temporary.appendingPathComponent(name)
        try? FileManager.default.removeItem(at: output)
        let helper = Process()
        helper.executableURL = executable
        helper.arguments = ["--convert", source.path, "\(tier.width)x\(tier.height)", output.path]
        helper.qualityOfService = .utility
        helper.standardOutput = FileHandle.nullDevice
        helper.standardError = FileHandle.nullDevice
        helper.terminationHandler = { [weak self] process in
            DispatchQueue.main.async { self?.finishFitted(process, source: source, output: output, name: name) }
        }
        do { try helper.run() } catch {
            refusedFits.insert(name)
            return
        }
        fitting = (helper, name)
        log("fitting \(name) from \(source.lastPathComponent)")
    }

    private func finishFitted(_ process: Process, source: URL, output: URL, name: String) {
        fitting = nil
        let manager = FileManager.default
        let expected = MP4.duration(source), produced = MP4.duration(output)
        let finished = process.terminationReason == .exit && process.terminationStatus == 0
        guard finished, expected > 0, abs(produced - expected) < 0.01 else {
            try? manager.removeItem(at: output)
            if process.terminationReason == .exit { refusedFits.insert(name) }
            log("fitting \(name) stopped: status \(process.terminationStatus), \(produced) s against \(expected) s")
            return
        }
        let destination = Library.variantsFolder.appendingPathComponent(name)
        try? manager.removeItem(at: destination)
        guard (try? manager.moveItem(at: output, to: destination)) != nil else {
            refusedFits.insert(name)
            return
        }
        pruneFitted(keeping: destination)
        library = Library.load(excluding: broken)
        log("fitted \(name): \(String(format: "%.3f", produced)) s, variants \(library.summary)")
        reconcile(immediate: true)
    }

    private func pruneFitted(keeping current: URL) {
        let manager = FileManager.default
        let files = ((try? manager.contentsOfDirectory(at: Library.variantsFolder, includingPropertiesForKeys: [.contentModificationDateKey])) ?? [])
            .filter { $0.pathExtension == "mp4" && $0 != current }
            .sorted {
                let a = (try? $0.resourceValues(forKeys: [.contentModificationDateKey]).contentModificationDate) ?? .distantPast
                let b = (try? $1.resourceValues(forKeys: [.contentModificationDateKey]).contentModificationDate) ?? .distantPast
                return a > b
            }
        for file in files.dropFirst(Int(WP_FITTED_LIMIT) - 1) {
            try? manager.removeItem(at: file)
            log("removed fitted variant \(file.lastPathComponent)")
        }
    }

    private func syncSystemWallpaper() {
        let area = canvas
        guard !area.isNull, area.width > 1 else { return }
        guard let content = background(for: canvasBacking, rate: rate) else { return }
        let signature = "\(content.url.path)|\(area)|\(Pref.text(Pref.fit))"
        guard signature != lastWallpaperSignature else { return }
        if wallpaperAttempts.signature != signature { wallpaperAttempts = (signature, 0) }
        guard wallpaperAttempts.count < 3 else { return }
        wallpaperAttempts.count += 1
        lastWallpaperSignature = signature
        guard let executable = Bundle.main.executableURL else { return }
        let helper = Process()
        helper.executableURL = executable
        helper.arguments = ["--wallpaper", content.url.path]
        helper.terminationHandler = { [weak self] process in
            guard process.terminationStatus != 0 else { return }
            DispatchQueue.main.async {
                guard let self else { return }
                log("system wallpaper not set; retrying in 5 seconds")
                self.lastWallpaperSignature = ""
                DispatchQueue.main.asyncAfter(deadline: .now() + 5) { self.syncSystemWallpaper() }
            }
        }
        try? helper.run()
    }

    private var rate: Double {
        var prefs = Pref.prefs
        var state = system
        return wp_rate(&prefs, &state)
    }

    private var system: wp_system {
        var state = wp_system()
        state.manual_pause = manualPause
        state.display_asleep = displayAsleep
        state.screensaver = screensaver
        state.locked = locked
        state.session_inactive = sessionInactive
        state.on_battery = power.onBattery
        state.low_power = power.lowPower
        state.hot = power.hot
        return state
    }

    @objc private func layoutMayHaveChanged() {
        followUp?.cancel()
        let later = DispatchWorkItem { [weak self] in
            self?.followUp = nil
            self?.updatePlayback()
        }
        followUp = later
        DispatchQueue.main.asyncAfter(deadline: .now() + WP_SETTLE_DELAY, execute: later)
        let since = Date().timeIntervalSince(lastCheck)
        guard since >= 0.3 else {
            guard coalesced == nil else { return }
            let work = DispatchWorkItem { [weak self] in
                self?.coalesced = nil
                self?.updatePlayback()
            }
            coalesced = work
            DispatchQueue.main.asyncAfter(deadline: .now() + (0.3 - since), execute: work)
            return
        }
        updatePlayback()
    }

    private func updatePlayback() {
        lastCheck = Date()
        var state = system
        let rate = self.rate
        if rate != lastRate, builtIn {
            reconcile(immediate: true)
            return
        }
        lastRate = rate
        let isFrozen = wp_frozen(&state)
        if isFrozen != lastFrozen {
            let first = lastFrozen == nil
            lastFrozen = isFrozen
            if first && !isFrozen {
            } else if isFrozen {
                log("frozen (paused=\(manualPause) hot=\(power.hot) display-off=\(displayAsleep || locked || screensaver || sessionInactive))")
            } else {
                log("unfrozen")
            }
        }
        var prefs = Pref.prefs
        let checkCoverage = !isFrozen && playback.isVideo && !surfaces.isEmpty && prefs.pause_when_covered
        let now = uptime()
        var want = WP_WANT_RELEASE
        for surface in surfaces.values {
            var covered = false
            if checkCoverage, let screen = surface.window.screen {
                covered = Coverage.isCovered(displayID: surface.displayID, screen: screen, ownPID: ownPID)
            }
            if covered != surface.covered {
                log("screen \(surface.displayID): covered=\(covered)")
            }
            surface.covered = covered
            want = wp_want_merge(want, wp_decide(&prefs, &state, covered))
        }
        let allowed = wp_fitted_allowed(&state)
        if let fitting, !allowed {
            log("fitting \(fitting.name) stopped: on battery, low power or hot")
            fitting.process.terminate()
        }
        if allowed != fitAllowed {
            fitAllowed = allowed
            if allowed, let background = playback.background { considerFitted(background) }
        }
        playback.step(want, rate: rate, now: now)
        scheduleWake(polling: checkCoverage)
    }

    private func scheduleWake(polling: Bool) {
        let now = uptime()
        let covered = surfaces.values.contains { $0.covered }
        var next = polling ? now + wp_coverage_poll(covered) : -1
        next = wp_next_wake(now, next, playback.releaseDeadline)
        next = wp_next_wake(now, next, playback.switchDeadline)
        wakeTimer?.invalidate()
        wakeTimer = nil
        let plan = next < 0 ? "none" : String(format: "in %.0f s", next - now)
        if plan != lastPlan {
            lastPlan = plan
            log("next wake: \(plan)")
        }
        guard next >= 0 else { return }
        let timer = Timer(timeInterval: max(0.05, next - now), repeats: false) { [weak self] _ in self?.tick() }
        timer.tolerance = polling && covered ? 0.1 : polling ? 2 : 1
        RunLoop.main.add(timer, forMode: .common)
        wakeTimer = timer
    }

    private func tick() {
        if playback.switchDeadline >= 0 {
            reconcile()
        } else {
            updatePlayback()
        }
    }

    @objc private func displaySleep() { displayAsleep = true; updatePlayback() }
    @objc private func displayWake() { displayAsleep = false; updatePlayback() }
    @objc private func sessionOut() { sessionInactive = true; updatePlayback() }
    @objc private func sessionIn() { sessionInactive = false; updatePlayback() }
    @objc private func screensaverStart() { screensaver = true; updatePlayback() }
    @objc private func screensaverStop() { screensaver = false; updatePlayback() }
    @objc private func screenLocked() { locked = true; updatePlayback() }
    @objc private func screenUnlocked() { locked = false; updatePlayback() }

    @objc private func chooseBackground() {
        guard !converting else { return }
        NSApp.activate(ignoringOtherApps: true)
        let panel = NSOpenPanel()
        panel.allowedContentTypes = [.image, .movie]
        panel.allowsMultipleSelection = false
        panel.canChooseDirectories = false
        panel.title = "Choose Background"
        panel.message = "Pick a video or an image to use as the desktop background."
        panel.prompt = "Use"
        guard panel.runModal() == .OK, let url = panel.url else { return }

        let type = try? url.resourceValues(forKeys: [.contentTypeKey]).contentType
        if type?.conforms(to: .image) == true {
            adopt(url, kind: "image")
        } else {
            examineVideo(url)
        }
    }

    @objc private func setFit(_ sender: NSMenuItem) {
        guard let value = sender.representedObject as? String else { return }
        Pref.set(Pref.fit, value)
        reconcile(immediate: true)
    }

    @objc private func togglePref(_ sender: NSMenuItem) {
        guard let key = sender.representedObject as? String else { return }
        Pref.set(key, !Pref.flag(key))
        updatePlayback()
    }

    @objc private func toggleLogin() {
        do {
            try Login.set(!Login.enabled)
        } catch {
            let alert = NSAlert()
            alert.messageText = "The login item could not be changed."
            alert.informativeText = error.localizedDescription
            alert.runModal()
        }
    }

    @objc private func useBuiltIn() {
        Pref.set(Pref.contentPath, "")
        Pref.set(Pref.contentKind, "")
        reconcile(immediate: true)
    }

    private func adopt(_ url: URL, kind: String) {
        Pref.set(Pref.contentPath, url.path)
        Pref.set(Pref.contentKind, kind)
        still.forget(url)
        reconcile(immediate: true)
        discardOldConversions(keeping: url)
    }

    private func discardOldConversions(keeping current: URL) {
        let folder = supportFolder()
        let files = (try? FileManager.default.contentsOfDirectory(at: folder, includingPropertiesForKeys: nil)) ?? []
        for file in files where file.lastPathComponent.hasPrefix("background-") && file != current {
            try? FileManager.default.removeItem(at: file)
            log("removed old conversion: \(file.lastPathComponent)")
        }
    }

    private var largestScreen: CGSize {
        NSScreen.screens
            .map { $0.convertRectToBacking($0.frame).size }
            .max { $0.width * $0.height < $1.width * $1.height } ?? CGSize(width: 1920, height: 1200)
    }

    private func examineVideo(_ url: URL) {
        let asset = AVURLAsset(url: url)
        Task { @MainActor in
            guard let track = try? await asset.loadTracks(withMediaType: .video).first,
                  let natural = try? await track.load(.naturalSize),
                  let transform = try? await track.load(.preferredTransform) else {
                let alert = NSAlert()
                alert.messageText = "That file cannot be used."
                alert.informativeText = "No readable video track was found in \(url.lastPathComponent)."
                alert.runModal()
                return
            }
            let oriented = natural.applying(transform)
            let size = CGSize(width: abs(oriented.width), height: abs(oriented.height))
            let screen = largestScreen
            let excess = (size.width * size.height) / max(1, screen.width * screen.height)
            guard excess > 1.3 else { return adopt(url, kind: "video") }

            let duration = (try? await asset.load(.duration).seconds) ?? 0
            let alert = NSAlert()
            alert.messageText = "This video is larger than your screen."
            alert.informativeText = """
                \(Int(size.width)) × \(Int(size.height)) against a \(Int(screen.width)) × \(Int(screen.height)) screen — \
                \(String(format: "%.1f", excess))× more pixels to decode for every frame, with no visible gain.
                A one-time conversion writes a screen-sized copy and leaves the original untouched.
                """
            alert.addButton(withTitle: "Convert")
            alert.addButton(withTitle: "Use As Is")
            alert.addButton(withTitle: "Cancel")
            if duration > 180 {
                alert.informativeText += "\n\nThe video is \(Int(duration / 60)) minutes long, so converting will take a while."
            }
            switch alert.runModal() {
            case .alertFirstButtonReturn: convert(url, to: screen)
            case .alertSecondButtonReturn: adopt(url, kind: "video")
            default: break
            }
        }
    }

    private func convert(_ url: URL, to size: CGSize) {
        converting = true
        status?.button?.title = " 0%"
        Transcoder.convert(url, to: size) { [weak self] fraction in
            self?.status?.button?.title = " \(Int(fraction * 100))%"
        } completion: { [weak self] result in
            guard let self else { return }
            self.converting = false
            self.status?.button?.title = ""
            switch result {
            case .success(let output):
                self.adopt(output, kind: "video")
            case .failure(let error):
                let alert = NSAlert()
                alert.messageText = "The conversion failed."
                alert.informativeText = error.localizedDescription
                alert.runModal()
            }
        }
    }

    private func buildMenu() {
        let item = NSStatusBar.system.statusItem(withLength: NSStatusItem.variableLength)
        if let url = Bundle.main.url(forResource: "MenuIcon", withExtension: "png"),
           let logo = NSImage(contentsOf: url), logo.size.height > 0 {
            let height: CGFloat = 18
            logo.size = NSSize(width: (height * logo.size.width / logo.size.height).rounded(), height: height)
            logo.isTemplate = false
            logo.accessibilityDescription = "Event Horizon"
            item.button?.image = logo
        } else {
            item.button?.image = NSImage(systemSymbolName: "circle.circle", accessibilityDescription: "Event Horizon")
        }
        item.button?.imagePosition = .imageLeft
        item.button?.toolTip = "Event Horizon"

        let menu = NSMenu()
        menu.delegate = self
        let title = NSMenuItem(title: "Event Horizon", action: nil, keyEquivalent: "")
        title.isEnabled = false
        menu.addItem(title)
        let state = NSMenuItem(title: "", action: nil, keyEquivalent: "")
        state.isEnabled = false
        menu.addItem(state)
        stateItem = state
        menu.addItem(.separator())

        let pause = NSMenuItem(title: "Pause", action: #selector(togglePause), keyEquivalent: "")
        pause.target = self
        menu.addItem(pause)
        pauseItem = pause
        let whenCovered = NSMenuItem(title: "Pause When Covered", action: #selector(togglePref(_:)), keyEquivalent: "")
        whenCovered.target = self
        whenCovered.representedObject = Pref.pauseWhenCovered
        menu.addItem(whenCovered)
        prefItems.append(whenCovered)
        menu.addItem(.separator())

        let choose = NSMenuItem(title: "Choose Video or Image…", action: #selector(chooseBackground), keyEquivalent: "")
        choose.target = self
        menu.addItem(choose)
        let builtIn = NSMenuItem(title: "Use Built-in Gargantua", action: #selector(useBuiltIn), keyEquivalent: "")
        builtIn.target = self
        menu.addItem(builtIn)
        contentItem = builtIn
        menu.addItem(.separator())

        menu.addItem(.sectionHeader(title: "Speed"))
        let speed = NSMenuItem()
        speed.view = speedRow
        speedRow.onChange = { [unowned self] value in
            Pref.set(Pref.speed, value)
            self.updatePlayback()
        }
        menu.addItem(speed)
        menu.addItem(.separator())

        menu.addItem(.sectionHeader(title: "Fit"))
        for (label, value) in [("Fill Screen", "fill"), ("Fit Inside", "fit")] {
            let entry = NSMenuItem(title: label, action: #selector(setFit(_:)), keyEquivalent: "")
            entry.target = self
            entry.representedObject = value
            menu.addItem(entry)
            fitItems.append(entry)
        }
        menu.addItem(.separator())

        menu.addItem(.sectionHeader(title: "Battery"))
        for (label, key) in [("Half Speed on Battery", Pref.halfSpeedOnBattery),
                             ("Half Speed in Low Power Mode", Pref.halfSpeedInLowPower)] {
            let entry = NSMenuItem(title: label, action: #selector(togglePref(_:)), keyEquivalent: "")
            entry.target = self
            entry.representedObject = key
            menu.addItem(entry)
            prefItems.append(entry)
        }
        menu.addItem(.separator())
        let login = NSMenuItem(title: "Open at Login", action: #selector(toggleLogin), keyEquivalent: "")
        login.target = self
        menu.addItem(login)
        loginItem = login
        menu.addItem(.separator())
        let quit = NSMenuItem(title: "Quit", action: #selector(quit), keyEquivalent: "q")
        quit.target = self
        menu.addItem(quit)
        item.menu = menu
        status = item
    }

    func menuNeedsUpdate(_ menu: NSMenu) {
        pauseItem?.title = manualPause ? "Resume" : "Pause"
        let rate = self.rate
        speedRow.speed = Pref.chosenSpeed
        prefItems.forEach { $0.state = Pref.flag($0.representedObject as? String ?? "") ? .on : .off }
        let custom = !builtIn
        contentItem?.state = custom || library.tiers.isEmpty ? .off : .on
        loginItem?.state = Login.enabled ? .on : .off
        let fit = Pref.text(Pref.fit)
        fitItems.forEach { $0.state = ($0.representedObject as? String) == fit ? .on : .off }

        let name: String
        if let background = playback.background {
            var tier = wp_tier()
            switch background {
            case .image(let url): name = url.lastPathComponent
            case .video(let url) where !custom && wp_tier_parse(url.path, &tier):
                name = "\(tier.width)×\(tier.height) · \(tier.fps) fps"
            case .video(let url): name = url.lastPathComponent
            }
        } else { name = library.tiers.isEmpty ? "Built-in video missing" : "Nothing to show" }

        let reason: String
        if converting { reason = "converting" }
        else if playback.background == nil { reason = "choose a video or image" }
        else if playback.background?.isVideo == false { reason = "still image" }
        else if manualPause { reason = "paused" }
        else if displayAsleep || locked || screensaver || sessionInactive { reason = "frozen, display off" }
        else if power.hot { reason = "frozen, running hot" }
        else if playback.state != WP_PLAYING && !surfaces.isEmpty { reason = "paused, covered" }
        else { reason = String(format: rate < Pref.chosenSpeed ? "playing %g×, half speed" : "playing %g×", rate) }
        stateItem?.title = "\(name) · \(reason)"
    }

    func validateMenuItem(_ item: NSMenuItem) -> Bool {
        item !== contentItem || !library.tiers.isEmpty
    }

    @objc private func togglePause() {
        manualPause.toggle()
        updatePlayback()
    }

    @objc private func quit() { NSApp.terminate(nil) }

    func applicationWillTerminate(_ notification: Notification) {
        wakeTimer?.invalidate()
        if let mouseMonitor { NSEvent.removeMonitor(mouseMonitor) }
        fitting?.process.terminate()
        playback.teardown()
        surfaces.values.forEach { $0.dispose() }
    }
}

if let flag = CommandLine.arguments.firstIndex(of: "--wallpaper") {
    guard let path = CommandLine.arguments.dropFirst(flag + 1).first else { exit(2) }
    Pref.register()
    exit(WallpaperWriter.run(URL(fileURLWithPath: path)) ? 0 : 1)
}

if let flag = CommandLine.arguments.firstIndex(of: "--convert") {
    let rest = CommandLine.arguments.dropFirst(flag + 1)
    guard let path = rest.first else {
        print("usage: --convert <file> [<width>x<height>] [<output>]")
        exit(2)
    }
    let source = URL(fileURLWithPath: path)
    let size: CGSize
    let sides = 16...Double(WP_DIMENSION_MAX)
    if rest.count > 1, let pair = rest.dropFirst().first?.split(separator: "x"), pair.count == 2,
       let w = Double(pair[0]), let h = Double(pair[1]), sides.contains(w), sides.contains(h) {
        size = CGSize(width: w, height: h)
    } else {
        let screen = NSScreen.screens.first
        size = screen.map { $0.convertRectToBacking($0.frame).size } ?? CGSize(width: 1920, height: 1200)
    }
    let output = rest.count > 2 ? URL(fileURLWithPath: Array(rest)[2]) : Transcoder.destination(for: source, target: size)
    print("\(source.lastPathComponent) -> \(Int(size.width))x\(Int(size.height))")
    var shown = -1
    do {
        let parent = getppid()
        try Transcoder.run(source, size, output) { fraction in
            if getppid() != parent { exit(3) }
            let percent = Int(fraction * 100)
            if percent != shown, percent % 5 == 0 {
                shown = percent
                print("  \(percent)%")
                fflush(stdout)
            }
        }
    } catch {
        print("failed: \(error.localizedDescription)")
        exit(1)
    }
    let bytes = (try? FileManager.default.attributesOfItem(atPath: output.path)[.size] as? Int) ?? nil
    print("written: \(output.path) (\(String(format: "%.1f", Double(bytes ?? 0) / 1_048_576)) MB)")
    exit(0)
}

let app = NSApplication.shared
app.setActivationPolicy(.accessory)
let engine = Engine()
app.delegate = engine
app.run()
