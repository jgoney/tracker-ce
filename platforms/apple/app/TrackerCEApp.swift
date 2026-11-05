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
        // Replies exactly once: normally when teardown finishes, else after 5 s (a hung audio
        // driver must not keep the process alive; the wedged teardown is deliberately leaked).
        let lock = NSLock()
        var replied = false
        let reply = {
            lock.lock(); defer { lock.unlock() }
            if replied { return }
            replied = true
            RunLoop.main.perform(inModes: [.common, .modalPanel]) {
                NSApp.reply(toApplicationShouldTerminate: true)
            }
        }
        AppModel.shared.shutdown(completion: reply)
        DispatchQueue.global().asyncAfter(deadline: .now() + 5) {
            lock.lock(); let already = replied; lock.unlock()
            if !already {
                print("shutdown watchdog: teardown still running after 5 s, quitting anyway")
                fflush(stdout); fflush(stderr)
                // AppKit's exit() would run C++ static destructors while the tracker/render
                // threads may still be running: leave without them.
                _exit(0)
            }
            reply()
        }
        return .terminateLater
    }

    /// Automation (documented in docs/apple_port.md): --screenshot PATH, --test-clock-play,
    /// --quit-after SECONDS.
    private func handleTestArguments(_ model: AppModel) {
        let a = CommandLine.arguments
        func value(_ k: String) -> String? { a.firstIndex(of: k).flatMap { $0 + 1 < a.count ? a[$0 + 1] : nil } }
        #if TRACKERCE_TEST_HOOKS
        if a.contains("--test-clock-play") {
            DispatchQueue.main.asyncAfter(deadline: .now() + 3) { model.bridge.testClockAndPlay() }
        }
        #endif
        if let path = value("--screenshot") {
            let delay = Double(value("--screenshot-after") ?? "6") ?? 6
            DispatchQueue.main.asyncAfter(deadline: .now() + delay) {
                let ok = model.bridge.saveScreenshot(path)
                print("screenshot \(ok ? "written" : "FAILED"): \(path)")
                model.showDebug = a.contains("--debug")
            }
        }
        #if TRACKERCE_TEST_HOOKS
        if a.contains("--test-key") {
            // Synthetic keyDown/keyUp for the '1' key (pad 0) through the window: proves the
            // MTKView is first responder and the press reaches InputApple.
            DispatchQueue.main.asyncAfter(deadline: .now() + 5) {
                guard let w = NSApp.windows.first(where: { $0.isVisible }) else { print("test-key: no window"); return }
                func ev(_ t: NSEvent.EventType) -> NSEvent? {
                    NSEvent.keyEvent(with: t, location: .zero, modifierFlags: [], timestamp: ProcessInfo.processInfo.systemUptime,
                                     windowNumber: w.windowNumber, context: nil, characters: "1",
                                     charactersIgnoringModifiers: "1", isARepeat: false, keyCode: 18)
                }
                NSApp.sendEvent(ev(.keyDown)!)
                DispatchQueue.main.asyncAfter(deadline: .now() + 0.3) {
                    let down = model.bridge.isPadDown(0)
                    NSApp.sendEvent(ev(.keyUp)!)
                    DispatchQueue.main.asyncAfter(deadline: .now() + 0.3) {
                        let up = !model.bridge.isPadDown(0)
                        print("test-key: pad0 down=\(down) released=\(up) firstResponder=\(String(describing: type(of: w.firstResponder as Any)))")
                    }
                }
            }
        }
        #endif
        if a.contains("--debug") { model.showDebug = true }
        if let s = value("--quit-after"), let t = Double(s) {
            DispatchQueue.main.asyncAfter(deadline: .now() + t) {
                print("underruns: \(model.bridge.underruns)")
                let now = model.bridge.takeTimingMaxUs().map { $0.uint32Value }
                let peak = zip(model.timingPeak, now).map { max($0, $1) }
                print("peak us: suspend \(peak[0]) render \(peak[1]) mask \(peak[2]) (budget 2900)")
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
