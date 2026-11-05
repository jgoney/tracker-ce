# Tracker CE on Apple Platforms — Mac MVP Design

Status: approved 2026-11-05; revised after independent review (same scope) · Date: 2026-11-05 · Branch: `apple-port-design` (local only, not pushed)

## 1. Intent and the why

The user doesn't own a Polyend Tracker. They want to **try the tracker way of sequencing**, which is new to them, with their modular. They also want to **build it themselves from this firmware** rather than pay for or use another tracker. Tracker CE is proven and well liked, and its sequencer, sample engine and UI are MIT-licensed.

The long-term picture is an iPad app. It plays drums and samples out through an Expert Sleepers ES-8 (a DC-coupled USB audio interface) into a small modular system. It also sends CV/gate through the ES-8 and MIDI to the user's own MIDI-to-CV module, Semaphore. A later stretch is live resampling: a rolling buffer of modular audio that pattern steps chop, stutter and re-sequence.

This document covers only the **first milestone: a Mac app that runs Tracker CE with its real screen.** Its job is to prove the core runs on Apple hardware, and to give the user something playable before any touch-UI design.

Priority: this is a fun/backlog project. It sits behind the user's other work (Undertow, Beacon, Kaiku). Nothing about it is urgent.

## 2. What the MVP is

A macOS app. Built from this repo's `core/`, it:

1. Shows the Tracker UI (800×480 logical) in a window, **rendered with Metal**. Every main screen is usable and recognizably right, including the pattern editor, sample loader/editor, instrument parameters, song/arrangement, mixer and config.
2. Takes **computer-keyboard input as stand-ins for the hardware pads, buttons and encoders**. That is enough to create a project, load samples, write patterns and play them.
3. **Plays the sample engine** through Core Audio to a user-selected output device. The ES-8 and the Mac's built-in output must both work.
4. **Sends MIDI** through CoreMIDI to a user-selected destination: notes, CC, program change, pitchbend, clock, start/stop. That way the MIDI tracks can drive Semaphore or any other MIDI-to-CV.
5. **Saves and loads projects and samples** in a folder on disk, which stands in for the SD card.

### Success looks like

The user opens the app and loads a few WAV drum samples from a folder. They write an 8-sample-track pattern plus a MIDI track, press play, and hear the drums through the ES-8 while the MIDI track plays a voice in the rack, in time. Then they save, quit, reopen and pick up where they left off.

## 3. Goals, non-goals, anti-goals

**Goals**
- The core runs unmodified wherever possible. Platform differences live behind the existing HAL interfaces in `core/include/Platform.h` and `core/include/DisplayDriver.h`, plus a new rendering backend.
- Metal renderer from day one. The user chose this to avoid a throwaway Core Graphics step and because it is the path to a polished iPad look.
- Timing that is good enough to sequence a modular: steady audio, and MIDI clock with no audible jitter.

**Non-goals for this milestone** (planned later stages, in rough order)
- iPad build, touch interface and UI redesign. The iPad must stay *possible*, so avoid macOS-only APIs in shared code.
- ES-8 CV/gate output tracks, with per-output calibration.
- Semaphore integration beyond plain MIDI (Tracker CC → Semaphore/Performer parameters).
- Live input buffer, chopping and stutter.
- Open replacement synth engines (e.g. Mutable Instruments, MIT) and AUv3 effect hosting.

**Out of scope entirely**
- The proprietary synth libraries (`synth/lib/libPolySynth.a`, `libPolyDSP.a`). They are Cortex-M7 object files, so they can't run on a Mac or iPad. (Their licence would allow personal non-commercial use, but that doesn't help here.) Never link them. (How this interacts with `FEATURE_SYNTH` and project compatibility is covered in section 5.)
- `.ptfx` Sample Editor plugins. They are ARM binaries loaded by `core/effector/ptfx_loader.cpp`, which writes MPU/cache registers. Stub them out.
- The Sample Recorder (recording from audio input). It is out of the MVP. Stub the screen so it can't crash. *(User's call.)* It is the **first stage after the MVP**, because it lays the groundwork for the live buffer.
- Hardware-only subsystems: radio, battery, USB audio, firmware update, factory test (`core/source/mtTest.cpp`), SGTL5000 codec control. Stub them out.
- Tracker Mini layout.

**Anti-goals** (failures even if it "works")
- Rewriting or forking the 41 screen files in `core/interface/` or the widget library in `core/libs/NewDisplay/` to make rendering work. The point is that the existing UI runs as is.
- Breaking the existing MK66/IMX hardware builds, or changing hardware behaviour without saying so. The user has no Tracker to test on.
- Anything that conflicts with the licence: no proprietary libs, no selling it, and no Polyend trademarks in the app name or icon. The licence explicitly allows a free, personal, non-commercial iOS/desktop port.
- Weeks of plumbing with nothing on screen. Get pixels up early.

## 4. What we know about the code (from recon)

**HAL boundary.** `core/` builds as a library (`core/makefile` → `libTracker.a`). Each platform supplies implementations of these:
- the abstract classes in `core/include/Platform.h`: `Platform`, `Midi`, `UsbMidi`, `UartMidi`, `Timer`, `SwitchGroup`, `Encoder`, `Leds`, `FileSystem`, `Sgtl5000`, `Radio`, `Battery`, `Usb`, `UsbAudio`, `CrcCalculator`
- `DisplayDriver` (`core/include/DisplayDriver.h`)
- the free functions in `core/source/interruptsControl.h` and `core/source/platform_math.h`

The entry points are in `core/include/Tracker.h`: `init()`, `update()` and `updateAudioBuffers()`. `platforms/imx/` is the reference implementation.

**Leaks to handle:**
- `core/makefile` hard-codes the Teensy/Cortex-M4 defines and flags. A host build needs its own build definition.
- `core/libs/Audio/utility/pdb.h` includes `kinetis.h`.
- `core/source/mtHardware.h` holds Kinetis pin macros.
- `dspinst.h` uses ARM asm under `KINETISK`, with C fallbacks.
- `__NOINIT(EXTERNAL_RAM)` and `AUDIO_CORE_RAM` section macros should become no-ops.
- `ptfx_loader.cpp` and `mtTest.cpp` touch registers directly. Exclude or stub them.
- There are about 107 `PLATFORM_IMX` and 81 `TRACKER_MINI` ifdefs. These are product feature flags, not register access. The Apple build should behave like the Tracker+ (IMX, PRODUCT=PLUS) feature set, with the synth handled as described in section 5.

**Display.** The UI is layered:
- Screens (`core/interface/`, 41 files) call only the widget API on `display` (`setControlText`, `refreshControl`, …). Only `mtTest.cpp` touches the chip directly.
- Widgets (`core/libs/NewDisplay/`, about 35 files) make about 1,600 calls to about 60 `API_*` functions. These are declared in `core/libs/FT812/FT812.h` and implemented in `FT812.cpp`. Each one encodes a 32-bit FT812 (Bridgetek EVE2) display-list word and writes it through `DisplayDriver::WriteData32`.
- The vocabulary is small. The most-used calls are `VERTEX2F`, `COLOR`, `BEGIN`/`END` (points, lines, rects, line strips), `LINE_WIDTH`, `SAVE`/`RESTORE_CONTEXT`, `CMD_TEXT`, `BLEND_FUNC`, bitmap setup (`BITMAP_HANDLE`/`SOURCE`/`LAYOUT`/`SIZE`, `CELL`, `VERTEX2II`), `SCISSOR`, `CMD_GRADIENT`, `CMD_NUMBER` and `CMD_APPEND`/`MEMCPY`/`SETBITMAP`.
- There is no `CMD_INFLATE`: fonts and images are RLE-decoded on the CPU. `CMD_SNAPSHOT2` is used only for screenshots, and `CMD_LOADIMAGE` only by the image viewer.
- Bitmap formats in use: L4, L2, L8, RGB565 and PALETTED4444. Rotated and scaled bitmaps need transforms. *(Corrected after independent review.)*
- Fonts are L4 bitmaps of Liberation Sans (`core/libs/fonts/`). Images and fonts are uploaded to the chip's RAM_G through `writeDataRAMG` / `writeFontRAMG`. `stream.cpp` uses DMA streaming into RAM_G.

**Input.** Platform `SwitchGroup` callbacks reach `onPadPush/Release/Hold`, `onButtonPush/Release/Hold` and `onPotChange` in `core/source/mtActions.cpp`. Pads have no velocity. Callbacks fire from the main-loop update, not from an interrupt.

**Audio and timing.**
- The audio is int16, in 128-sample blocks (`AUDIO_BLOCK_SAMPLES`), at 44100 Hz on IMX. It uses a modified Teensy Audio library in `core/libs/Audio`.
- The **sequencer clock runs inside the audio callback**: `sequencer.tick(len/sr)` in `Tracker::updateAudioBuffers()` (`core/source/Tracker.cpp` around line 171).
- Outgoing MIDI clock is placed within a block through `midiSetClockSubBlockOffsetUs` → `Timer::scheduleMidiClockUs`, which is a hardware one-shot on IMX.

**MIDI.** All MIDI goes through `core/source/mtMidi.{h,cpp}` to the `Midi`/`UsbMidi`/`UartMidi` interfaces. Notes leave the sequencer through `sendNoteOn`/`sendNoteOff` (`mtSequencer.cpp` around lines 2421 and 2472). That is the future hook for ES-8 CV tracks.

**Memory.** The static pools are large: a 60 MB sample pool on IMX (`SAMPLE_MEMORY_SIZE` in `core/source/mtStructs.h`), a 768 KB scratch buffer and a 1.5 MB delay buffer. All of this is fine on a Mac. Section placement comes from linker scripts, not fixed addresses.

**Tests.** None exist. googletest and catch2 are vendored in `util/3rdparty/` but unused.

## 5. Decisions made in conversation

- **Platform:** Apple-native. A C++ core with a thin Swift/SwiftUI app shell, bridged with Objective-C++. No React Native or other cross-platform UI layer. *(User agreed.)*
- **Mac first, iPad later,** from one codebase. The Mac build is where the work gets proven. The user has an iPad Pro (a few years old) and prior mobile-dev experience, though not iPad-specific. *(User agreed.)*
- **No paid Apple developer account for now.** That is fine for the Mac build. Free iPad provisioning expires every 7 days, which the user accepts for later. *(User agreed.)*
- **Metal renderer from the start**, not Core Graphics first. *(User's call.)*
- **The MVP includes the real Tracker screen**, not a headless player. *(User's call.)*
- **The visual target** in the long run is "between" the stock Tracker look and its own character. For the MVP, faithful reproduction of the stock look is enough.
- **Keep projects compatible with Tracker+.** The MVP's project files and instrument numbering must match a real Tracker+, so shared projects open and MVP projects survive the later open-synth stage.
  - Today `FEATURE_SYNTH` controls two things at once: the synth instrument slots and data layout (`kTotalInstrumentsCount` in `core/source/mtStructs.h:108`, `LIMITER_SIDECHAIN_MAX`, file manager, sequencer, MIDI; about 40 files), and the use of the proprietary libs.
  - Split it so the Apple build keeps the slots, layout and UI but links no proprietary code. Stub the synth host, or back it with the open CHIP engine if that's practical.
  - Synth tracks that would need a proprietary engine load, save and display normally but play silently.
  - The split must not change the MK66 or IMX hardware builds. *(User agreed to my recommendation.)*
- **Sample Recorder out of the MVP,** first thing after. *(User agreed to my recommendation.)*
- **MIDI timing gets a later tightening pass.** The MVP implements latency-compensated timestamping and a manual offset (section 6). Precise targets and measurement come later. *(User's call.)*
- **Changes to the core are welcome when they're the right fix** (user decision, 2026-11-05). In order of preference:
  1. A fix that is correct on every platform. Example: build paths with `FileSystem::kPathSeparator` instead of the backslashes in `core/synth/Synth.cpp:462-464`.
  2. A compile-time platform trait (`if constexpr`) when behaviour must differ.
  3. `#ifdef PLATFORM_APPLE` only where code can't compile on one side.

  Core changes go in small, separate commits marked *neutral* or *hardware-visible*, so they could become upstream PRs. Hardware-visible ones need community testing first (`CONTRIBUTING.md` §4). The user can borrow a friend's Tracker for hardware testing, so hardware-visible changes are testable when needed. Batch them for a borrowing session.
- **Audio interface:** Core Audio. **MIDI:** CoreMIDI. **Storage:** a folder on disk. *(My call, stated to the user.)*

## 6. Calls I made (the user should check these)

- **Rendering seam: decode at the `DisplayDriver` level** (revised after builder review). Implement an Apple `DisplayDriver` that models the parts of the FT812 the Tracker uses:
  - a RAM_G byte array, the display list, and the coprocessor command stream
  - a decoder for the display-list words and coprocessor commands the code actually emits
  - a small renderer interface with a Metal backend that draws the decoded primitives

  Leave `FT812.cpp`, `NewDisplay` and `interface/` unmodified.

  *Why:* the display code caches drawing on the chip. `core/libs/NewDisplay/display.cpp` (around lines 565 and 654) and `arrangement.cpp` record each widget's commands into `RAM_DL`, copy them to RAM_G with `CMD_MEMCPY`, and replay them each frame with `CMD_APPEND`. `snapshots.cpp` uses `CMD_SNAPSHOT2`. Swapping the `API_*` functions for direct draw calls would therefore still need record/replay at RAM_G addresses, so decoding the words is the cleaner seam.

  Inventory of what the decoder must cover:
  - the display-list opcodes that `FT812.cpp` encodes
  - the coprocessor commands in use (`CMD_TEXT`, `CMD_NUMBER`, `CMD_GRADIENT`, `CMD_APPEND`, `CMD_MEMCPY`, `CMD_SETBITMAP`, `CMD_SETBASE`, `CMD_LOADIMAGE`, `CMD_SNAPSHOT2`, `CMD_DLSTART`/`CMD_SWAP`, …)
  - the EVE bitmap formats actually used (at least L4 for fonts)
  - the DMA stream path in `stream.cpp`

  **Coprocessor commands write display-list words**, exactly as on EVE, and only the swap turns the display list into shapes. This applies to text, numbers and gradients too. Widgets cache their *expanded* display list (`display.cpp:557`, read via `getDisplayListOffset()`) and replay it, so drawing shapes early would drop text from every cached widget. *(Found in independent review.)*

  The coprocessor always reports itself as empty, and swaps happen synchronously.
- **MIDI timing.** Every outgoing MIDI message, notes and CC as well as clock, is timestamped with CoreMIDI timestamps to the moment its audio block will actually be heard. That moment is block time plus the device's reported output latency, plus the core's existing sub-block offset where one exists. That keeps MIDI-driven voices aligned with the drums. Add a user-adjustable MIDI offset in ms (±) to absorb the extra delay of Semaphore and the voice itself. For the MVP, MIDI-driven voices should sound in time with the drums by ear, and clock should drive a clocked module steadily. Tightening with real measurement is deferred to a later pass (section 5).
- **MIDI input is out of the MVP.** The Tracker is the clock master. Following external clock and recording notes from a controller are later work. Keep incoming MIDI harmless: ignore it, or pass it to the core without timing guarantees.
- **Code location.** A new `platforms/apple/` directory, mirroring `platforms/imx/`. It holds the HAL implementations, the renderer and the Xcode project. Any core edits are minimal and ifdef'd.
- **Feature set.** Tracker+ (IMX, PLUS) behaviour, without `FEATURE_SYNTH`.
- **Sample rate.** 44.1 kHz, 128-frame blocks, so the core runs exactly as on hardware. If the output device runs at 48 kHz, ask the device for 44.1 kHz first (the ES-8 supports it). Resample only as a fallback.
- **Version control.** The work lives on local branches of this clone. Whether it becomes a public fork is the user's decision later. The CE guidelines encourage forks for "radical experiments".

## 7. Left to the builder

- The host build system for `core/` (Xcode targets, CMake or a host makefile) and how the Apple app links it.
- The exact keyboard map for the 48 pads, function buttons and encoder. It should be documented in the app (a help overlay is fine) and roughly follow the hardware layout.
- The threading model for the main-loop `update()` versus the Core Audio render thread. It must match the hardware's assumptions: the main loop runs at a non-interrupt priority, and the audio callback must never block.
- How the Metal renderer batches primitives, handles scaling (integer or smooth) and anti-aliasing. Points with radius, lines with width, scissor and blend modes must look right.
- Device pickers for audio output and MIDI destination (a minimal menu is fine).
- Whether to add a few host unit tests using the vendored catch2/googletest, now that a host build exists. Encouraged where cheap, for example sequencer timing and the API_* → renderer translation.
- Where the "SD card" folder lives (app container vs. a user-chosen folder).

## 8. Risks and unknowns

- **Bitmap formats and coprocessor commands.** `CMD_SNAPSHOT2`, `CMD_LOADIMAGE`, DMA streaming (`stream.cpp`) and any EVE bitmap formats beyond L4 are the least-known part of the renderer work. Inventory them first.
- **Hidden hardware assumptions.** Timing loops, `millis()`/`micros()` from the Teensy core, `elapsedMillis`, and any busy-waits on display "coprocessor empty" flags. The Apple `DisplayDriver` should report the coprocessor as always empty.
- **Two interrupt-masking mechanisms.** Besides the audio-interrupt pair, `core/config/config.h` has `DISABLE_GLOBAL_INTERRUPTS` (inline `CPSID i`). `AudioStream` allocation calls it from both the audio and the main side. Both pairs must share one host mechanism, and both must be no-ops when called from the audio thread itself.
- **Global initialisation order.** Many globals (`engine`, `sequencer`, `display`, `mtProject`, …) assume a hardware boot sequence (`core/source/Tracker.cpp` `init()`).
- **iPad later.** Keep shared code free of AppKit. Use only Metal, Core Audio, CoreMIDI and Foundation below the app shell.

## 9. Later stages (for context, not for this plan)

0. **Sample Recorder.** Core Audio input, choosing an ES-8 input channel, and the microphone permission. This is the foundation for the live buffer.
1. **iPad build + touch UI.** Use the visual companion for interface design. Consider swapping bitmap fonts for vector Liberation Sans for crisp scaling.
2. **ES-8 CV/gate tracks.** A CV output backend at the `sendNoteOn`/`sendNoteOff` choke point, with per-output volt/oct calibration and CC/envelope → CV. Budget: 8 outputs, with stereo audio taking 2.
3. **Tracker ↔ Semaphore.** CC/NRPN mapping to Performer parameters on the Semaphore side, coordinated with the Semaphore firmware sessions.
4. **Live buffer.** A rolling capture of an ES-8 input as an instrument, sliced and triggered by steps, with roll/stutter FX and a freeze control.
5. **Open sound engines and AUv3 effects.**
6. **base16 themes.** Tracker CE already has JSON colour themes (`/Themes/*.json`), custom `.ptfont` fonts and a web theme editor (`docs/custom_themes_and_fonts.md`). The Mac app inherits all of this from the core. The add-on is a base16 YAML → Tracker theme JSON converter, so any scheme from [tinted-theming/base16-schemes](https://github.com/tinted-theming/base16-schemes) works. That mirrors the user's WaveEdit fork (`~/dev/WaveEdit_clones/WaveEdit/src/theme.cpp`, `doc/maintenance/2026-04-09-themes-fonts-design.md`). Needs its own short design for the base16 → Tracker key mapping.
