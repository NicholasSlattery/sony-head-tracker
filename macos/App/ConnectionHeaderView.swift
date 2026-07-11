import SwiftUI

struct ConnectionHeaderView: View {
    let tracker: TrackerModel

    var body: some View {
        HStack(spacing: 12) {
            Image(systemName: symbolName)
                .foregroundStyle(statusColor)
                .accessibilityHidden(true)

            VStack(alignment: .leading, spacing: 2) {
                Text(statusTitle)
                    .font(.headline)
                Text(statusDetail)
                    .font(.callout)
                    .foregroundStyle(.secondary)
            }

            Spacer()

            Button(tracker.isRunning ? "Stop" : "Start") {
                if tracker.isRunning {
                    tracker.stop()
                } else {
                    tracker.start()
                }
            }
            .buttonStyle(.borderedProminent)
        }
        .padding()
        .background(.quaternary, in: .rect(cornerRadius: 12))
        .accessibilityElement(children: .combine)
    }

    private var statusTitle: String {
        switch tracker.connectionState {
        case .stopped: "Stopped"
        case .scanning: "Scanning"
        case .connected: "Connected"
        case .reconnecting: "Reconnecting"
        case .permissionDenied: "Permission required"
        case .failed: "Error"
        }
    }

    private var statusDetail: String {
        switch tracker.connectionState {
        case .stopped:
            "Start tracking when a compatible headset is connected."
        case .scanning:
            "Looking for a verified Android Head Tracker."
        case let .connected(device):
            device
        case let .reconnecting(message),
             let .permissionDenied(message),
             let .failed(message):
            message
        }
    }

    private var symbolName: String {
        switch tracker.connectionState {
        case .connected: "checkmark.circle.fill"
        case .scanning, .reconnecting: "arrow.clockwise"
        case .permissionDenied, .failed: "exclamationmark.triangle.fill"
        case .stopped: "stop.circle"
        }
    }

    private var statusColor: Color {
        switch tracker.connectionState {
        case .connected: .green
        case .scanning, .reconnecting: .orange
        case .permissionDenied, .failed: .red
        case .stopped: .secondary
        }
    }
}
