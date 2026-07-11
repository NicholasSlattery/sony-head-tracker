import SwiftUI

struct DeviceInspectorView: View {
    let gyroscope: SIMD3<Double>?
    let accelerometer: SIMD3<Double>?
    let diagnosticsText: String

    var body: some View {
        VStack(alignment: .leading, spacing: 8) {
            Text("Device inspector")
                .font(.headline)
            LabeledContent("Usage", value: "0x0020:0x00E1")
            LabeledContent("Marker", value: "#AndroidHeadTracker#")
            LabeledContent("Gyroscope", value: gyroscope == nil ? "Unavailable" : "Available")
            LabeledContent("Accelerometer", value: accelerometer == nil ? "Unavailable" : "Available")
            DisclosureGroup("Shareable diagnostics") {
                ScrollView {
                    Text(diagnosticsText)
                        .font(.caption.monospaced())
                        .textSelection(.enabled)
                        .frame(maxWidth: .infinity, alignment: .leading)
                }
                .frame(maxHeight: 180)
            }
        }
        .frame(maxWidth: .infinity, alignment: .leading)
        .padding()
        .background(.quaternary, in: .rect(cornerRadius: 10))
    }
}
