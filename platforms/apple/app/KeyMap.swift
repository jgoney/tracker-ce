import AppKit

/// What a physical key does. Keys are identified by hardware key code (ANSI positions), so the
/// map is the same on every keyboard layout.
enum KeyAction: Equatable {
    case pad(Int)
    case button(Int)
    case encoder(Int) // +-1 (Shift: x10)
}

enum KeyMap {
    // 48 pad keys in pad-index order. Pads 0-31 are the 4x8 grid (rows 1-4), 32-47 the 16 keys.
    // Row r uses 12 physical keys (cols 0-7 -> grid pads r*8+0..7, cols 8-11 -> key pads r*4+0..3).
    static let rows: [[(UInt16, String)]] = [
        [(18, "1"), (19, "2"), (20, "3"), (21, "4"), (23, "5"), (22, "6"), (26, "7"), (28, "8"),
         (25, "9"), (29, "0"), (27, "-"), (24, "=")],
        [(12, "Q"), (13, "W"), (14, "E"), (15, "R"), (17, "T"), (16, "Y"), (32, "U"), (34, "I"),
         (31, "O"), (35, "P"), (33, "["), (30, "]")],
        [(0, "A"), (1, "S"), (2, "D"), (3, "F"), (5, "G"), (4, "H"), (38, "J"), (40, "K"),
         (37, "L"), (41, ";"), (39, "'"), (42, "\\")],
        [(6, "Z"), (7, "X"), (8, "C"), (9, "V"), (11, "B"), (45, "N"), (46, "M"), (43, ","),
         (47, "."), (44, "/"), (50, "`"), (10, "§")],
    ]

    /// (key code, label) per pad index 0..47.
    static let padKeys: [(UInt16, String)] = {
        var grid: [(UInt16, String)] = []
        var keys: [(UInt16, String)] = []
        for row in rows {
            grid += row[0..<8]
            keys += row[8..<12]
        }
        return grid + keys
    }()

    static let padForKey: [UInt16: Int] = {
        var m: [UInt16: Int] = [:]
        for (i, k) in padKeys.enumerated() { m[k.0] = i }
        return m
    }()

    // Direct buttons (no modifier). Tracker button numbers: interfaceDefs.h.
    static let directButtons: [(UInt16, String, Int, String)] = [
        (126, "Up", 26, "Up"), (125, "Down", 31, "Down"), (123, "Left", 30, "Left"),
        (124, "Right", 32, "Right"), (49, "Space", 23, "Play"), (48, "Tab", 29, "Shift"),
        (51, "Delete", 27, "Delete"), (36, "Return", 25, "Insert"), (53, "Esc", 28, "Rec"),
        (117, "Fwd-Delete", 24, "Copy"),
    ]
    // F1...F12 -> buttons 8...19 (Params ... Instr).
    static let functionKeys: [UInt16] = [122, 120, 99, 118, 96, 97, 98, 100, 101, 109, 103, 111]

    static let buttonNames: [String] = {
        var n = (0..<44).map { "Button \($0)" }
        let named: [Int: String] = [
            8: "Params", 9: "Performance", 10: "File", 11: "Config", 12: "Master",
            13: "Sample Load", 14: "Sample Play", 15: "Sample Edit", 16: "Sample Rec", 17: "Song",
            18: "Note", 19: "Instr", 20: "Vol", 21: "Fx", 22: "Pattern", 23: "Play", 24: "Copy",
            25: "Insert", 26: "Up", 27: "Delete", 28: "Rec", 29: "Shift", 30: "Left", 31: "Down",
            32: "Right", 33: "Mini Middle", 34: "Menu", 35: "Fn1", 36: "Fn2", 37: "Fn3",
            38: "Fn4", 39: "+10", 40: "+1", 41: "-1", 42: "-10", 43: "Game",
        ]
        for (k, v) in named { n[k] = v }
        return n
    }()

    // Encoder: Page Up/Down, keypad +/-, Option+, / Option+. (all x10 with Shift); scroll too.
    static let encoderKeys: [UInt16: Int] = [116: 1, 121: -1, 69: 1, 78: -1]
    static let optionEncoderKeys: [UInt16: Int] = [47: 1, 43: -1]

    static func action(keyCode: UInt16, flags: NSEvent.ModifierFlags) -> KeyAction? {
        let f = flags.intersection([.control, .option, .command])
        if f.contains(.command) { return nil }
        if f.contains(.control) {
            if let p = padForKey[keyCode], p < 44 { return .button(p) }
            return nil
        }
        if f.contains(.option) {
            if let d = optionEncoderKeys[keyCode] { return .encoder(d) }
            return nil
        }
        if let d = encoderKeys[keyCode] { return .encoder(d) }
        if let b = directButtons.first(where: { $0.0 == keyCode }) { return .button(b.2) }
        if let i = functionKeys.firstIndex(of: keyCode) { return .button(8 + i) }
        if let p = padForKey[keyCode] { return .pad(p) }
        return nil
    }

    static var helpText: [(String, [String])] {
        var pads: [String] = []
        for (r, row) in rows.enumerated() {
            let g = row[0..<8].map { $0.1 }.joined(separator: " ")
            let k = row[8..<12].map { $0.1 }.joined(separator: " ")
            pads.append("Row \(r + 1):  \(g)   |   \(k)")
        }
        return [
            ("Pads (grid | key pads)", pads),
            ("Buttons", directButtons.map { "\($0.1) = \($0.3)" } + [
                "F1-F12 = Params, Performance, File, Config, Master, Sample Load/Play/Edit/Rec, Song, Note, Instr",
                "Control + pad key N = button N (all 44; e.g. Ctrl+1 = button 0, Ctrl+Q = button 8)",
            ]),
            ("Encoder", [
                "Page Up / Page Down, keypad + / -, Option+. / Option+, : +1 / -1",
                "Hold Shift for +-10. Trackpad / mouse scroll: +-1 per step (Shift: +-10)",
            ]),
            ("App", ["Cmd+/ : this overlay   Cmd+D : debug overlay   Cmd+, : settings"]),
        ]
    }
}

/// Tracks key-down state: auto-repeat never re-sends a press, a release always matches the action
/// chosen at press time (even if a modifier changed in between), losing focus releases everything.
final class KeyRouter {
    private let bridge: TCBridge
    private var down: [UInt16: KeyAction] = [:]
    private var scrollAccum: CGFloat = 0

    init(bridge: TCBridge) { self.bridge = bridge }

    @discardableResult
    func keyDown(_ e: NSEvent) -> Bool {
        if e.isARepeat { return true }
        if down[e.keyCode] != nil { return true }
        guard let a = KeyMap.action(keyCode: e.keyCode, flags: e.modifierFlags) else { return false }
        switch a {
        case .pad(let i): bridge.postPad(Int32(i), down: true)
        case .button(let i): bridge.postButton(Int32(i), down: true)
        case .encoder(let d):
            bridge.postEncoder(Int32(e.modifierFlags.contains(.shift) ? d * 10 : d))
            return true // each press clicks once; the OS repeat is ignored above
        }
        down[e.keyCode] = a
        return true
    }

    @discardableResult
    func keyUp(_ e: NSEvent) -> Bool {
        guard let a = down.removeValue(forKey: e.keyCode) else { return false }
        release(a)
        return true
    }

    func releaseAll() {
        for a in down.values { release(a) }
        down.removeAll()
    }

    private func release(_ a: KeyAction) {
        switch a {
        case .pad(let i): bridge.postPad(Int32(i), down: false)
        case .button(let i): bridge.postButton(Int32(i), down: false)
        case .encoder: break
        }
    }

    func scroll(_ e: NSEvent) {
        let step: CGFloat = e.hasPreciseScrollingDeltas ? 14 : 1
        scrollAccum += e.scrollingDeltaY
        let clicks = Int((scrollAccum / step).rounded(.towardZero))
        if clicks != 0 {
            scrollAccum -= CGFloat(clicks) * step
            bridge.postEncoder(Int32(e.modifierFlags.contains(.shift) ? clicks * 10 : clicks))
        }
    }
}
