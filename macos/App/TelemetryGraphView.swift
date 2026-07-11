import SwiftUI

struct TelemetryGraphView: View {
    let history: [OrientationPoint]

    var body: some View {
        VStack(alignment: .leading, spacing: 8) {
            Text("Recent orientation")
                .font(.headline)
            Canvas { context, size in
                draw(history.map(\.yaw), color: .blue, context: &context, size: size)
                draw(history.map(\.pitch), color: .green, context: &context, size: size)
                draw(history.map(\.roll), color: .orange, context: &context, size: size)
            }
            .frame(height: 150)
            .background(.quaternary, in: .rect(cornerRadius: 10))
            HStack {
                GraphLegend(label: "Yaw", color: .blue)
                GraphLegend(label: "Pitch", color: .green)
                GraphLegend(label: "Roll", color: .orange)
            }
        }
        .accessibilityElement(children: .ignore)
        .accessibilityLabel("Recent orientation graph")
        .accessibilityValue(summary)
    }

    private var summary: String {
        guard let latest = history.last else { return "No samples" }
        return "Latest yaw \(latest.yaw.formatted(.number.precision(.fractionLength(1)))) degrees, "
            + "pitch \(latest.pitch.formatted(.number.precision(.fractionLength(1)))) degrees, "
            + "roll \(latest.roll.formatted(.number.precision(.fractionLength(1)))) degrees"
    }

    private func draw(
        _ values: [Double],
        color: Color,
        context: inout GraphicsContext,
        size: CGSize
    ) {
        guard values.count > 1 else { return }
        var path = Path()
        for (index, value) in values.enumerated() {
            let x = size.width * CGFloat(index) / CGFloat(values.count - 1)
            let normalized = max(-180, min(180, value)) / 360 + 0.5
            let point = CGPoint(x: x, y: size.height * CGFloat(1 - normalized))
            if index == 0 { path.move(to: point) } else { path.addLine(to: point) }
        }
        context.stroke(path, with: .color(color), lineWidth: 1.5)
    }
}

private struct GraphLegend: View {
    let label: String
    let color: Color

    var body: some View {
        Label {
            Text(label)
        } icon: {
            Image(systemName: "circle.fill")
                .foregroundStyle(color)
        }
        .font(.caption)
    }
}
