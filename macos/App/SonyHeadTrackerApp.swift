import SwiftUI

@main
struct SonyHeadTrackerApp: App {
    @State private var tracker = TrackerModel()

    var body: some Scene {
        WindowGroup {
            ContentView(tracker: tracker)
                .frame(minWidth: 760, minHeight: 560)
        }
        .commands {
            CommandMenu("Tracking") {
                Button("Recenter") {
                    tracker.recenter()
                }
                .keyboardShortcut("r", modifiers: .command)
                .disabled(!tracker.isRunning)

            }
        }

        Settings {
            SettingsView(tracker: tracker)
                .frame(width: 460, height: 420)
        }
    }
}
