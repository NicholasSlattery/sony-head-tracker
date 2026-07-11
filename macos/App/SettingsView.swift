import SwiftUI

struct SettingsView: View {
    @Bindable var tracker: TrackerModel

    var body: some View {
        Form {
            Section("Tracking") {
                Picker("Axis order", selection: $tracker.axisOrder) {
                    ForEach(["XYZ", "XZY", "YXZ", "YZX", "ZXY", "ZYX"], id: \.self) {
                        Text($0)
                    }
                }
                HStack {
                    Toggle("Invert X", isOn: $tracker.invertX)
                    Toggle("Invert Y", isOn: $tracker.invertY)
                    Toggle("Invert Z", isOn: $tracker.invertZ)
                }
                Slider(value: $tracker.smoothing, in: 0.01...1.0) {
                    Text("Smoothing")
                } minimumValueLabel: {
                    Text("0.01")
                } maximumValueLabel: {
                    Text("1.0")
                }
                Text("Smoothing: \(tracker.smoothing.formatted(.number.precision(.fractionLength(2))))")
                    .font(.caption)
                    .foregroundStyle(.secondary)
            }
            .onChange(of: tracker.axisOrder) { tracker.applyFilterSettings() }
            .onChange(of: tracker.invertX) { tracker.applyFilterSettings() }
            .onChange(of: tracker.invertY) { tracker.applyFilterSettings() }
            .onChange(of: tracker.invertZ) { tracker.applyFilterSettings() }
            .onChange(of: tracker.smoothing) { tracker.applyFilterSettings() }

            Section("Output") {
                TextField("Base UDP port", value: $tracker.udpPort, format: .number)
                Text("Port changes apply the next time tracking starts. JSON uses the next port.")
                    .font(.caption)
                    .foregroundStyle(.secondary)
            }
            .onChange(of: tracker.udpPort) { tracker.saveSettings() }

            Section("Open-source license") {
                ThirdPartyLicensesView()
            }
        }
        .formStyle(.grouped)
        .padding()
    }
}
