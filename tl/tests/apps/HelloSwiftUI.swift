import SwiftUI

@main
struct HelloApp: App {
    var body: some Scene {
        WindowGroup { ContentView() }
    }
}

struct ContentView: View {
    @State private var taps = 0
    @State private var name = ""
    var body: some View {
        VStack(spacing: 12) {
            Text("Hello from a Mac app").font(.title)
            TextField("Name", text: $name).frame(width: 200)
            Button("Tapped \(taps) times") { taps += 1 }
            List(0..<5, id: \.self) { Text("Row \($0)") }.frame(height: 120)
        }
        .padding()
        .frame(minWidth: 320, minHeight: 300)
        .onAppear {
            guard ProcessInfo.processInfo.environment["TL_SELFTEST"] != nil else { return }
            DispatchQueue.main.asyncAfter(deadline: .now() + 0.5) {
                let windows = NSApplication.shared.windows.filter(\.isVisible)
                print("SwiftUI window shown: \(windows.count) visible, bundle \(Bundle.main.bundleIdentifier ?? "(none)")")
                print("PASS HelloSwiftUI")
                exit(0)
            }
        }
    }
}
