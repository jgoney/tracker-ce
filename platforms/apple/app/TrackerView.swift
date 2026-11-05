import AppKit
import MetalKit
import SwiftUI

/// MTKView that draws the FT812 model's latest frame at 60 Hz and feeds the keyboard to KeyRouter.
final class TrackerMTKView: MTKView {
    var router: KeyRouter?

    override var acceptsFirstResponder: Bool { true }
    override func viewDidMoveToWindow() {
        super.viewDidMoveToWindow()
        window?.makeFirstResponder(self)
        // SwiftUI may reassign the first responder after the window appears: claim it again.
        DispatchQueue.main.async { [weak self] in self?.window?.makeFirstResponder(self) }
        NotificationCenter.default.removeObserver(self)
        NotificationCenter.default.addObserver(forName: NSWindow.didBecomeKeyNotification,
                                               object: window, queue: .main) { [weak self] _ in
            guard let self = self else { return }
            self.window?.makeFirstResponder(self)
        }
        NotificationCenter.default.addObserver(forName: NSWindow.didResignKeyNotification,
                                               object: window, queue: .main) { [weak self] _ in
            self?.router?.releaseAll()
        }
    }
    override func keyDown(with e: NSEvent) {
        if e.keyCode == 53 && AppModel.shared.showKeyMap { AppModel.shared.showKeyMap = false; return }
        if router?.keyDown(e) != true { super.keyDown(with: e) }
    }
    override func keyUp(with e: NSEvent) { if router?.keyUp(e) != true { super.keyUp(with: e) } }
    override func scrollWheel(with e: NSEvent) { router?.scroll(e) }
    override func mouseDown(with e: NSEvent) { window?.makeFirstResponder(self) }
}

struct TrackerView: NSViewRepresentable {
    let model: AppModel

    func makeCoordinator() -> Coordinator { Coordinator(model.bridge) }

    func makeNSView(context: Context) -> TrackerMTKView {
        let v = TrackerMTKView()
        v.device = model.bridge.device
        v.colorPixelFormat = model.bridge.pixelFormat
        v.clearColor = MTLClearColor(red: 0, green: 0, blue: 0, alpha: 1)
        v.preferredFramesPerSecond = 60
        v.framebufferOnly = true
        v.router = model.router
        v.delegate = context.coordinator
        return v
    }

    func updateNSView(_ v: TrackerMTKView, context: Context) {}

    final class Coordinator: NSObject, MTKViewDelegate {
        let bridge: TCBridge
        var queue: MTLCommandQueue?
        init(_ b: TCBridge) {
            bridge = b
            queue = b.device?.makeCommandQueue()
        }
        func mtkView(_ view: MTKView, drawableSizeWillChange size: CGSize) {}
        func draw(in view: MTKView) {
            guard let pass = view.currentRenderPassDescriptor,
                  let drawable = view.currentDrawable,
                  let cb = queue?.makeCommandBuffer(),
                  let enc = cb.makeRenderCommandEncoder(descriptor: pass) else { return }
            bridge.draw(in: enc, size: view.drawableSize) // device pixels (Retina)
            enc.endEncoding()
            cb.present(drawable)
            cb.commit()
        }
    }
}

struct ContentView: View {
    @ObservedObject var m = AppModel.shared

    var body: some View {
        ZStack(alignment: .topLeading) {
            TrackerView(model: m)
            if m.showDebug {
                VStack(alignment: .leading, spacing: 2) {
                    Text("underruns: \(m.underruns)").foregroundStyle(m.underruns == 0 ? Color.green : Color.red)
                    Text("blocks: \(m.blocks)   latency: \(String(format: "%.1f", m.latencyMs)) ms")
                    Text(m.audioStatus)
                }
                .font(.system(size: 11, design: .monospaced)).padding(6)
                .background(.black.opacity(0.7)).foregroundStyle(.white).padding(6)
            }
            if m.showKeyMap {
                VStack(alignment: .leading, spacing: 8) {
                    ForEach(KeyMap.helpText, id: \.0) { sec in
                        Text(sec.0).font(.headline)
                        ForEach(sec.1, id: \.self) { Text($0).font(.system(size: 12, design: .monospaced)) }
                    }
                    Text("Cmd+/ or Esc to close").font(.caption).foregroundStyle(.secondary)
                }
                .padding(16)
                .background(.black.opacity(0.88)).foregroundStyle(.white)
                .cornerRadius(8).padding(24)
                .frame(maxWidth: .infinity, maxHeight: .infinity)
            }
        }
        .frame(minWidth: 800, minHeight: 480)
        .background(Color.black)
    }
}
