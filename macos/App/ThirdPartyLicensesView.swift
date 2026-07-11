import SwiftUI

struct ThirdPartyLicensesView: View {
    var body: some View {
        VStack(alignment: .leading, spacing: 8) {
            Text("Sony Head Tracker")
                .font(.headline)
            Text("MIT licensed. Unofficial and not affiliated with or endorsed by Sony.")
            if let repositoryURL = URL(
                string: "https://github.com/NicholasSlattery/sony-head-tracker"
            ) {
                Link("NicholasSlattery/sony-head-tracker", destination: repositoryURL)
            }
            Text(Self.licenseText)
                .font(.caption.monospaced())
                .textSelection(.enabled)
                .frame(maxHeight: 120)
        }
    }

    private static let licenseText: String = {
        guard let url = Bundle.main.url(
            forResource: "sony-head-tracker-LICENSE",
            withExtension: "txt"
        ) else {
            return "License resource is unavailable."
        }
        return (try? String(contentsOf: url, encoding: .utf8))
            ?? "License resource could not be read."
    }()
}
