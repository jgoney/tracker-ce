import AppKit
import SwiftUI

final class AppDelegate: NSObject, NSApplicationDelegate {
    func applicationDidFinishLaunching(_ n: Notification) {
        let model = AppModel.shared
        model.start()
        handleTestArguments(model)
    }

    func applicationShouldTerminateAfterLastWindowClosed(_ app: NSApplication) -> Bool { true }

    func applicationShouldTerminate(_ app: NSApplication) -> NSApplication.TerminateReply {
        AppModel.shared.shutdown { NSApp.reply(toApplicationShouldTerminate: true) }
        return .terminateLater
    }

    /// Automation (documented in docs/apple_port.md): --screenshot PATH, --test-clock-play,
    /// --quit-after SECONDS.
    private func handleTestArguments(_ model: AppModel) {
        let a = CommandLine.arguments
        func value(_ k: String) -> String? { a.firstIndex(of: k).flatMap { $0 + 1 < a.count ? a[$0 + 1] : nil } }
        if a.contains("--test-clock-play") {
            DispatchQueue.main.asyncAfter(deadline: .now() + 3) { model.bridge.testClockAndPlay() }
        }
        if let path = value("--screenshot") {
            let delay = Double(value("--screenshot-after") ?? "6") ?? 6
            DispatchQueue.main.asyncAfter(deadline: .now() + delay) {
                let ok = model.bridge.saveScreenshot(path)
                print("screenshot \(ok ? "written" : "FAILED"): \(path)")
                model.showDebug = a.contains("--debug")
            }
        }
        if a.contains("--debug") { model.showDebug = true }
        if let s = value("--quit-after"), let t = Double(s) {
            DispatchQueue.main.asyncAfter(deadline: .now() + t) {
                print("underruns: \(model.bridge.underruns)")
                NSApp.terminate(nil)
            }
        }
    }
}

@main
struct TrackerCEApp: App {
    @NSApplicationDelegateAdaptor(AppDelegate.self) var delegate
    @ObservedObject var model = AppModel.shared

    var body: some Scene {
        Window("Tracker CE (unofficial)", id: "main") {
            ContentView()
        }
        .defaultSize(width: 800, height: 480)
        .commands {
            CommandGroup(replacing: .newItem) {
                Button("Choose SD Folder…") { model.chooseSDFolder() }
                Button("Save Screenshot…") { model.saveScreenshot() }
            }
            CommandGroup(after: .toolbar) {
                Button(model.showDebug ? "Hide Debug Overlay" : "Show Debug Overlay") { model.showDebug.toggle() }
                    .keyboardShortcut("d", modifiers: .command)
            }
            CommandGroup(replacing: .help) {
                Button(model.showKeyMap ? "Hide Keyboard Map" : "Keyboard Map") { model.showKeyMap.toggle() }
                    .keyboardShortcut("/", modifiers: .command)
            }
        }
        Settings { SettingsView() }
    }
}
