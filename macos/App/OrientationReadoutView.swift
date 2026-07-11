import SwiftUI

struct OrientationReadoutView: View {
    let tracker: TrackerModel

    var body: some View {
        HStack(spacing: 12) {
            AngleValueView(label: "Yaw", value: tracker.yaw)
            AngleValueView(label: "Pitch", value: tracker.pitch)
            AngleValueView(label: "Roll", value: tracker.roll)
            Spacer()
            Button("Recenter", systemImage: "scope") {
                tracker.recenter()
            }
            .disabled(!tracker.isRunning)
        }
        .accessibilityElement(children: .contain)
        .accessibilityLabel("Head orientation")
    }
}

private struct AngleValueView: View {
    let label: String
    let value: Double

    var body: some View {
        VStack(alignment: .leading, spacing: 4) {
            Text(label)
                .font(.caption)
                .foregroundStyle(.secondary)
            Text(value, format: .number.precision(.fractionLength(2)))
                .font(.system(.title2, design: .monospaced, weight: .semibold))
            Text("degrees")
                .font(.caption2)
                .foregroundStyle(.secondary)
        }
        .frame(minWidth: 120, alignment: .leading)
        .padding()
        .background(.quaternary, in: .rect(cornerRadius: 10))
        .accessibilityElement(children: .ignore)
        .accessibilityLabel(label)
        .accessibilityValue("\(value.formatted(.number.precision(.fractionLength(2)))) degrees")
    }
}
