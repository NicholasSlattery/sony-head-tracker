import SwiftUI

struct OutputStatusView: View {
    let basePort: UInt16
    let packetsPerSecond: Double

    var body: some View {
        VStack(alignment: .leading, spacing: 8) {
            Text("UDP output")
                .font(.headline)
            LabeledContent("OpenTrack", value: "127.0.0.1:\(basePort)")
            LabeledContent("JSON", value: "127.0.0.1:\(basePort + 1)")
            LabeledContent(
                "Packet rate",
                value: "\(packetsPerSecond.formatted(.number.precision(.fractionLength(1)))) pps"
            )
        }
        .frame(maxWidth: .infinity, alignment: .leading)
        .padding()
        .background(.quaternary, in: .rect(cornerRadius: 10))
    }
}
