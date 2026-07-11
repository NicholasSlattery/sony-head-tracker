import SwiftUI

struct ContentView: View {
    let tracker: TrackerModel

    var body: some View {
        ScrollView {
            VStack(alignment: .leading, spacing: 16) {
                ConnectionHeaderView(tracker: tracker)
                OrientationReadoutView(tracker: tracker)
                TelemetryGraphView(history: tracker.history)
                HStack(alignment: .top, spacing: 16) {
                    OutputStatusView(
                        basePort: tracker.udpPort,
                        packetsPerSecond: tracker.packetsPerSecond
                    )
                    DeviceInspectorView(
                        gyroscope: tracker.gyroscope,
                        accelerometer: tracker.accelerometer,
                        diagnosticsText: tracker.diagnosticsText
                    )
                }
            }
            .padding(20)
        }
        .onDisappear {
            tracker.stop()
        }
    }
}
