import AppKit
import SwiftUI

/// App-wide model: owns the bridge, persists settings in UserDefaults, keeps the UI state.
final class AppModel: ObservableObject {
    static let shared = AppModel()
    static let defaultSDRoot = ("~/Documents/Tracker CE" as NSString).expandingTildeInPath

    private let defaults = UserDefaults.standard
    let bridge: TCBridge
    let router: KeyRouter

    @Published var audioDevices: [String] = ["System Default"]
    @Published var midiDestinations: [String] = []     // display names
    @Published var midiDestinationKeys: [String] = []   // identity keys, index for index
    @Published var audioStatus: String = "Starting…"
    @Published var devicesLoading = true
    @Published var showKeyMap = false
    @Published var showDebug = false
    @Published var underruns: UInt64 = 0
    // Worst stage times (µs): last 0.25 s and since launch. [suspend, render, maskWait]
    @Published var timingNow: [UInt32] = [0, 0, 0]
    @Published var timingPeak: [UInt32] = [0, 0, 0]
    @Published var blocks: UInt64 = 0
    @Published var latencyMs: Double = 0

    @Published var audioDevice: String { didSet { defaults.set(audioDevice, forKey: "audioDevice") } }
    @Published var leftChannel: Int { didSet { defaults.set(leftChannel, forKey: "leftChannel") } }
    @Published var rightChannel: Int { didSet { defaults.set(rightChannel, forKey: "rightChannel") } }
    @Published var midiDestination: String { didSet { defaults.set(midiDestination, forKey: "midiDestination") } }
    @Published var midiOffsetMs: Int {
        didSet {
            defaults.set(midiOffsetMs, forKey: "midiOffsetMs")
            bridge.midiOffsetMs = Int32(midiOffsetMs)
        }
    }

    let sdRoot: String
    private var timer: Timer?

    private init() {
        let stored = UserDefaults.standard.string(forKey: "sdRoot")
        sdRoot = stored ?? AppModel.defaultSDRoot
        bridge = TCBridge(sdRoot: sdRoot)
        router = KeyRouter(bridge: bridge)
        audioDevice = defaults.string(forKey: "audioDevice") ?? ""
        leftChannel = defaults.object(forKey: "leftChannel") as? Int ?? 0
        rightChannel = defaults.object(forKey: "rightChannel") as? Int ?? 1
        midiDestination = defaults.string(forKey: "midiDestination") ?? ""
        midiOffsetMs = min(50, max(-50, defaults.integer(forKey: "midiOffsetMs")))
        bridge.midiOffsetMs = Int32(midiOffsetMs)
        bridge.setPreferredAudioDevice(audioDevice, left: Int32(leftChannel), right: Int32(rightChannel))
        bridge.setPreferredMidiDestination(midiDestination)
        bridge.onAudioStatus = { [weak self] s in
            DispatchQueue.main.async { self?.audioStatus = s }
        }
        // HAL device-list and CoreMIDI setup changes: the bridge refreshed off-main, publish here.
        bridge.onDevicesChanged = { [weak self] in
            guard let self = self else { return }
            self.publishDevices()
            self.applyMidi() // re-resolve the persisted choice (a replugged device reattaches)
        }
    }

    func start() {
        bridge.start()
        refreshDevices()
        timer = Timer.scheduledTimer(withTimeInterval: 0.25, repeats: true) { [weak self] _ in
            guard let self = self else { return }
            self.underruns = self.bridge.underruns
            self.blocks = self.bridge.blocksRendered
            self.latencyMs = self.bridge.outputLatencyMs
            let t = self.bridge.takeTimingMaxUs().map { $0.uint32Value }
            self.timingNow = t
            self.timingPeak = zip(self.timingPeak, t).map { max($0, $1) }
        }
    }

    func refreshDevices() {
        devicesLoading = true
        bridge.refreshDevices { [weak self] in
            self?.publishDevices()
            self?.devicesLoading = false
        }
    }

    /// Main thread: copies the bridge's cached lists (no HAL / CoreMIDI access).
    private func publishDevices() {
        audioDevices = bridge.audioDevices()
        midiDestinations = bridge.midiDestinations()
        midiDestinationKeys = bridge.midiDestinationKeys()
    }

    /// Picker entries for the MIDI destination: the live list, plus the saved choice when that
    /// device is currently absent (so the picker keeps showing it).
    var midiChoices: [(key: String, name: String)] {
        var out = zip(midiDestinationKeys, midiDestinations).map { (key: $0, name: $1) }
        if !midiDestination.isEmpty, !out.contains(where: { $0.key == midiDestination }) {
            let name = midiDestination.firstIndex(of: "|").map { String(midiDestination[midiDestination.index(after: $0)...]) } ?? midiDestination
            out.append((key: midiDestination, name: name + " (not connected)"))
        }
        return out
    }

    var audioDeviceIndex: Int { max(0, audioDevices.firstIndex(of: audioDevice) ?? 0) }
    var channelCount: Int { max(1, Int(bridge.audioChannelCount(at: audioDeviceIndex))) }

    /// Applies the current device + channel choice off the main thread.
    func applyAudio() {
        let name = audioDevice
        let n = channelCount
        let l = min(leftChannel, n - 1), r = min(rightChannel, n - 1)
        audioStatus = "Opening…"
        DispatchQueue.global(qos: .userInitiated).async { [bridge] in
            _ = bridge.selectAudioDeviceNamed(name, left: Int32(l), right: Int32(r))
        }
    }

    func applyMidi() {
        // By identity (unique id, falling back to name), resolved by the bridge at apply time.
        bridge.selectMidiDestinationKey(midiDestination)
    }

    func chooseSDFolder() {
        let p = NSOpenPanel()
        p.canChooseDirectories = true
        p.canChooseFiles = false
        p.canCreateDirectories = true
        p.prompt = "Choose"
        p.message = "Choose the folder that stands in for the Tracker's SD card."
        p.directoryURL = URL(fileURLWithPath: sdRoot)
        guard p.runModal() == .OK, let url = p.url else { return }
        defaults.set(url.path, forKey: "sdRoot")
        let a = NSAlert()
        a.messageText = "SD folder changed"
        a.informativeText = "\(url.path)\n\nThe new folder is used the next time Tracker CE starts."
        a.runModal()
    }

    func saveScreenshot() {
        let p = NSSavePanel()
        p.nameFieldStringValue = "tracker.png"
        p.allowedContentTypes = [.png]
        if p.runModal() == .OK, let url = p.url { _ = bridge.saveScreenshot(url.path) }
    }

    /// Quit teardown, off the main thread (a hung audio driver can take ~4 s).
    func shutdown(completion: @escaping () -> Void) {
        timer?.invalidate()
        router.releaseAll()
        DispatchQueue.global(qos: .userInitiated).async { [bridge] in
            bridge.stop()
            // RunLoop, not GCD: terminate() may call us from a nested loop inside a main-queue block.
            RunLoop.main.perform(inModes: [.common, .modalPanel], block: completion)
        }
    }
}

struct SettingsView: View {
    @ObservedObject var m = AppModel.shared

    var body: some View {
        Form {
            Section("Audio output") {
                Picker("Device", selection: $m.audioDevice) {
                    Text("System Default").tag("")
                    ForEach(m.audioDevices.dropFirst(), id: \.self) { Text($0).tag($0) }
                }
                .disabled(m.devicesLoading)
                Picker("Left channel", selection: $m.leftChannel) {
                    ForEach(0..<m.channelCount, id: \.self) { Text("Output \($0 + 1)").tag($0) }
                }
                Picker("Right channel", selection: $m.rightChannel) {
                    ForEach(0..<m.channelCount, id: \.self) { Text("Output \($0 + 1)").tag($0) }
                }
                Text(m.audioStatus).font(.callout).foregroundStyle(.secondary)
                    .textSelection(.enabled)
            }
            Section("MIDI output") {
                Picker("Destination", selection: $m.midiDestination) {
                    Text("None (virtual source only)").tag("")
                    ForEach(m.midiChoices, id: \.key) { Text($0.name).tag($0.key) }
                }
                .disabled(m.devicesLoading)
                HStack {
                    Slider(value: Binding(get: { Double(m.midiOffsetMs) },
                                          set: { m.midiOffsetMs = Int($0.rounded()) }),
                           in: -50...50, step: 1) { Text("Timing offset") }
                    Text("\(m.midiOffsetMs) ms").monospacedDigit().frame(width: 60, alignment: .trailing)
                }
                Text("Always publishes the virtual MIDI source \"Tracker CE (unofficial)\".")
                    .font(.callout).foregroundStyle(.secondary)
            }
            Section("SD card folder") {
                Text(m.sdRoot).textSelection(.enabled)
                Button("Choose Folder…") { m.chooseSDFolder() }
            }
            Button("Rescan devices") { m.refreshDevices() }.disabled(m.devicesLoading)
        }
        .formStyle(.grouped)
        .frame(width: 480, height: 520)
        .onChange(of: m.audioDevice) { _ in m.applyAudio() }
        .onChange(of: m.leftChannel) { _ in m.applyAudio() }
        .onChange(of: m.rightChannel) { _ in m.applyAudio() }
        .onChange(of: m.midiDestination) { _ in m.applyMidi() }
    }
}
