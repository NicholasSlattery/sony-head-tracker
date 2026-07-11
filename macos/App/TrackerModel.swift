import Foundation
import Observation

struct OrientationPoint: Identifiable, Equatable, Sendable {
    let id: UInt64
    let yaw: Double
    let pitch: Double
    let roll: Double
}

private struct SampleSnapshot: Sendable {
    let yaw: Double
    let pitch: Double
    let roll: Double
    let packetsPerSecond: Double
    let gyroscope: SIMD3<Double>?
    let accelerometer: SIMD3<Double>?

    init(_ sample: SHTSample) {
        yaw = sample.ypr_degrees.0
        pitch = sample.ypr_degrees.1
        roll = sample.ypr_degrees.2
        packetsPerSecond = sample.packets_per_second
        gyroscope = sample.has_gyroscope
            ? SIMD3(sample.gyroscope.0, sample.gyroscope.1, sample.gyroscope.2)
            : nil
        accelerometer = sample.has_accelerometer
            ? SIMD3(sample.accelerometer.0, sample.accelerometer.1, sample.accelerometer.2)
            : nil
    }
}

private struct StatusSnapshot: Sendable {
    let rawValue: UInt32
    let message: String
}

// The C callbacks receive this relay rather than an unretained TrackerModel
// pointer. Its model reference is read only on MainActor, and EngineHandle keeps
// the relay alive until sht_destroy has stopped and joined the native worker.
private final class CallbackRelay: @unchecked Sendable {
    weak var model: TrackerModel?
}

private final class EngineHandle: @unchecked Sendable {
    let raw: OpaquePointer?
    let relay = CallbackRelay()

    init() {
        raw = sht_create()
    }

    deinit {
        if let raw {
            sht_destroy(raw)
        }
    }
}

private func trackerSampleCallback(
    _ sample: UnsafePointer<SHTSample>?,
    _ context: UnsafeMutableRawPointer?
) {
    guard let sample, let context else { return }
    let snapshot = SampleSnapshot(sample.pointee)
    let relay = Unmanaged<CallbackRelay>.fromOpaque(context).takeUnretainedValue()
    Task { @MainActor [weak relay] in
        relay?.model?.accept(snapshot)
    }
}

private func trackerStatusCallback(
    _ status: SHTStatus,
    _ message: UnsafePointer<CChar>?,
    _ context: UnsafeMutableRawPointer?
) {
    guard let context else { return }
    let snapshot = StatusSnapshot(
        rawValue: status.rawValue,
        message: message.map(String.init(cString:)) ?? ""
    )
    let relay = Unmanaged<CallbackRelay>.fromOpaque(context).takeUnretainedValue()
    Task { @MainActor [weak relay] in
        relay?.model?.accept(snapshot)
    }
}

@MainActor
@Observable
final class TrackerModel {
    enum ConnectionState: Equatable {
        case stopped
        case scanning
        case connected(device: String)
        case reconnecting(message: String)
        case permissionDenied(message: String)
        case failed(message: String)
    }

    private(set) var connectionState: ConnectionState = .stopped
    private(set) var yaw = 0.0
    private(set) var pitch = 0.0
    private(set) var roll = 0.0
    private(set) var packetsPerSecond = 0.0
    private(set) var gyroscope: SIMD3<Double>?
    private(set) var accelerometer: SIMD3<Double>?
    private(set) var history: [OrientationPoint] = []
    private(set) var isRunning = false
    private(set) var diagnosticsText = "Diagnostics are not available yet."

    var smoothing = 0.18
    var udpPort: UInt16 = 4242
    var axisOrder = "YXZ"
    var invertX = true
    var invertY = false
    var invertZ = true

    @ObservationIgnored private let engine = EngineHandle()
    @ObservationIgnored private var nextPointID: UInt64 = 0
    @ObservationIgnored private var nextDiagnosticsRefresh = Date.distantPast

    init() {
        engine.relay.model = self
        loadSettings()
        if engine.raw == nil {
            connectionState = .failed(message: "Could not create the tracking engine.")
        }
        refreshDiagnostics(force: true)
    }

    func start() {
        guard !isRunning, let handle = engine.raw else { return }
        applyFilterSettings()
        let context = Unmanaged.passUnretained(engine.relay).toOpaque()
        if sht_start(
            handle,
            udpPort,
            trackerSampleCallback,
            trackerStatusCallback,
            context
        ) {
            isRunning = true
            connectionState = .scanning
        } else {
            connectionState = .failed(
                message: "Could not start tracking. Check the UDP port and try again."
            )
        }
    }

    func stop() {
        guard let handle = engine.raw else { return }
        sht_stop(handle)
        isRunning = false
        connectionState = .stopped
        packetsPerSecond = 0
    }

    func recenter() {
        guard isRunning, let handle = engine.raw else { return }
        sht_recenter(handle)
    }

    func applyFilterSettings() {
        guard let handle = engine.raw else { return }
        let source = axisOrder.map { axis in
            switch axis {
            case "X": 0 as UInt32
            case "Y": 1 as UInt32
            default: 2 as UInt32
            }
        }
        guard source.count == 3 else { return }
        let signs = [invertX ? -1.0 : 1.0,
                     invertY ? -1.0 : 1.0,
                     invertZ ? -1.0 : 1.0]
        source.withUnsafeBufferPointer { sourceBuffer in
            signs.withUnsafeBufferPointer { signBuffer in
                sht_set_filter(handle, smoothing, sourceBuffer.baseAddress, signBuffer.baseAddress)
            }
        }
        saveSettings()
    }

    func saveSettings() {
        let source = axisOrder.map { axis in
            switch axis {
            case "X": 0 as UInt32
            case "Y": 1 as UInt32
            default: 2 as UInt32
            }
        }
        guard source.count == 3 else { return }
        var config = SHTConfig()
        config.smoothing = smoothing
        config.udp_port = udpPort
        config.axis_source.0 = source[0]
        config.axis_source.1 = source[1]
        config.axis_source.2 = source[2]
        config.axis_sign.0 = invertX ? -1 : 1
        config.axis_sign.1 = invertY ? -1 : 1
        config.axis_sign.2 = invertZ ? -1 : 1
        _ = sht_save_config(&config)
    }

    fileprivate func accept(_ sample: SampleSnapshot) {
        guard isRunning else { return }
        yaw = sample.yaw
        pitch = sample.pitch
        roll = sample.roll
        packetsPerSecond = sample.packetsPerSecond
        gyroscope = sample.gyroscope
        accelerometer = sample.accelerometer
        history.append(OrientationPoint(
            id: nextPointID,
            yaw: sample.yaw,
            pitch: sample.pitch,
            roll: sample.roll
        ))
        nextPointID &+= 1
        if history.count > 300 {
            history.removeFirst(history.count - 300)
        }
        refreshDiagnostics()
    }

    fileprivate func accept(_ status: StatusSnapshot) {
        switch status.rawValue {
        case UInt32(SHT_STATUS_STOPPED.rawValue):
            isRunning = false
            connectionState = .stopped
        case UInt32(SHT_STATUS_SCANNING.rawValue):
            connectionState = .scanning
        case UInt32(SHT_STATUS_CONNECTED.rawValue):
            let prefix = "Connected: "
            let device = status.message.hasPrefix(prefix)
                ? String(status.message.dropFirst(prefix.count))
                : status.message
            connectionState = .connected(device: device)
        case UInt32(SHT_STATUS_RECONNECTING.rawValue):
            connectionState = .reconnecting(message: status.message)
        case UInt32(SHT_STATUS_PERMISSION_DENIED.rawValue):
            isRunning = false
            connectionState = .permissionDenied(message: status.message)
        case UInt32(SHT_STATUS_NOT_VISIBLE.rawValue):
            connectionState = .reconnecting(message: "Not visible: \(status.message)")
        case UInt32(SHT_STATUS_NOT_VERIFIED.rawValue):
            connectionState = .failed(message: "Not verified: \(status.message)")
        case UInt32(SHT_STATUS_FEATURE_WRITE_FAILED.rawValue):
            connectionState = .failed(message: "Feature configuration: \(status.message)")
        case UInt32(SHT_STATUS_STREAM_TIMEOUT.rawValue):
            connectionState = .reconnecting(message: "Stream timeout: \(status.message)")
        case UInt32(SHT_STATUS_UDP_ERROR.rawValue):
            isRunning = false
            connectionState = .failed(message: "UDP: \(status.message)")
        default:
            isRunning = false
            connectionState = .failed(message: status.message)
        }
        refreshDiagnostics(force: true)
    }

    private func refreshDiagnostics(force: Bool = false) {
        guard let handle = engine.raw else { return }
        let now = Date()
        guard force || now >= nextDiagnosticsRefresh else { return }
        nextDiagnosticsRefresh = now.addingTimeInterval(1)
        let required = sht_get_diagnostics(handle, nil, 0)
        guard required > 1 else { return }
        var buffer = [CChar](repeating: 0, count: max(required + 2048, 8192))
        let written = buffer.withUnsafeMutableBufferPointer { pointer in
            sht_get_diagnostics(handle, pointer.baseAddress, pointer.count)
        }
        guard written > 1, written <= buffer.count else { return }
        diagnosticsText = String(
            decoding: buffer.prefix { $0 != 0 }.map { UInt8(bitPattern: $0) },
            as: UTF8.self
        )
    }

    private func loadSettings() {
        var config = SHTConfig()
        guard sht_load_config(&config) else { return }
        let source = [config.axis_source.0, config.axis_source.1, config.axis_source.2]
        guard Set(source).count == 3, source.allSatisfy({ $0 < 3 }) else { return }
        axisOrder = String(source.map {
            $0 == 0 ? Character("X") : $0 == 1 ? Character("Y") : Character("Z")
        })
        smoothing = config.smoothing
        udpPort = config.udp_port
        invertX = config.axis_sign.0 < 0
        invertY = config.axis_sign.1 < 0
        invertZ = config.axis_sign.2 < 0
    }
}
