# Apple port

## Core changes

Changes to `core/` made for the Apple host build. *neutral* means the hardware code is unchanged. Verification so far: `core/` objects only (`make -C core build` for MK66/ORIGINAL, IMX/PLUS, IMX/MINI) built on `main` and on this branch with a shimmed GCC 15.3 (the only runnable arm toolchain here), debug info stripped: 697 of 699 objects identical, the exception being `synth/Synth.o` as expected. **That result predates commits fe7d28c (play_memory return) and d523436 (project buffer zero-init)**, so it must be re-run. In a real hardware `cmp` the expected differing objects are `Synth.o`, the three play_memory objects (`play_memory_loop_pingpong.o`, `play_memory_single_shot.o`, `play_memory_wavetable.o`) and `fileManagerProject.o`; anything else is a surprise. The full `make original plus mini` build and `cmp` of the images with GCC 10.3 is still pending (needs Rosetta or the `agt` toolchain).

Policy: core edits only when right, each in its own `core:` commit, listed here as neutral or hardware-visible; host-only code sits behind `#if defined(PLATFORM_APPLE)`. A shared `PlatformTraits.h` (tier-2 platform predicates for portable core code) will be added when first needed; the unused one was removed.

- `core: host stubs for interrupt control and ELF section macros under PLATFORM_APPLE` (`config.h`, `cr_section_macros.h`) - *neutral*.
- `core: ordinary static storage for external RAM buffers on Apple (Mach-O sections)` (`Tracker.cpp`, `effect_polyverb.cpp`) - *neutral*.
- `core: skip mallinfo/malloc.h heap probing in Profiler on Apple` - *neutral*.
- `core: build synth patch paths with FileSystem::kPathSeparator` (`Synth.cpp`) - *neutral (string change)*: `\` becomes `/`, both accepted by FatFs. Changes `Synth.o`.
- `core: use uintptr_t for pointer-difference casts (64-bit hosts)` (`mtConfig.cpp`, `mtFileManagerOldVersionRecovery.cpp`, `mtAudioEngine.cpp`) - *neutral* (`uintptr_t` is 32 bits on Cortex-M).
- `core: const-correct strchr results in textBox (libc++ overloads)` - *neutral*.
- `core: use class-scope enumerators in fSampleRec case labels (clang requires constants)` - *neutral*.
- `core: portable C fallback for Polyverb Clip16 off ARM` - *neutral* (`#if defined(__arm__)` keeps the `ssat` asm).
- `core: portable C implementations of Cortex-M DSP intrinsics in dspinst.h for Apple` - *neutral* (hardware path untouched, new block under `PLATFORM_APPLE`).
- `core: include <cassert> in mtStructs.h under PLATFORM_APPLE` - *neutral* (inside the `PLATFORM_APPLE` block; the `SampleAddress` handle asserts).
- `core: keep strInstrument layout on 64-bit hosts via 4-byte SampleAddress handle` (`mtStructs.h`) - *neutral* (hardware keeps `int16_t*`).
- `core: return a value on the unreachable tail of AudioPlayMemory update paths` (`play_memory_loop_pingpong.cpp`, `play_memory_single_shot.cpp`, `play_memory_wavetable.cpp`) - **hardware-visible (code bytes; behaviour-neutral, unreachable tail)**: the fall-through is only reachable if `playing` is not 0 or 1, which never happens; it now releases the block and returns `nullptr` like the `!playing` branch. Fixes `-Wreturn-type` UB on the host.
- `core: zero-initialise project file buffer before save` (`fileManagerProject.cpp`) - **hardware-visible (benign)**: the stack `strProjectFile` was written to disk with the unused tails of `projectName`/`projectPath` holding stack garbage; now zero. Only bytes after the NUL terminators change, and the files stay readable by existing firmware. Makes project files deterministic (byte-identical re-saves).

## Needs hardware test

- Save-As a project that has synth tracks on a Tracker and confirm the patches are copied (the `Synth.cpp` path-separator change).
- Zero-initialised project buffer: save a project on the Tracker, reload it, and confirm it loads with its name intact.
- Display look: compare `trackerce-headless --metal` screenshots (start screen, pattern editor) with
  photos of a real Tracker+, in particular the panel pixel aspect (the 160x172 boot logo), RECTS
  border widths and CMD_NUMBER hex case. See `platforms/apple/display/INVENTORY.md` (Task 9).

## Known limitations

- **Synth patch edits are not saved.** The Mac build uses a silent PolySynth stand-in
  (`platforms/apple/synth_stub/`). Loading a patch keeps its raw bytes and saving writes them back
  unchanged, so patches round-trip with hardware, but any parameter or macro edit made to a loaded
  patch on the Mac is silently dropped on save. A new patch (no source file) is saved as a
  zero-length file, which hardware cannot load.
- **Patch folder layout is unconfirmed.** The folder-structured store uses `Patches/<typeId>/<name>.patch`
  (decimal instrument type id) and the flat store uses `<name>.patch`. This is a best guess from the
  PolySynth headers, not checked against a real SD card, so a hardware `Patches/` tree may not be listed.
- **Streamed samples moved or replaced while loaded.** The virtual SD sector map reopens a sample by
  path when its descriptor is evicted from a 32-entry cache. If the file was renamed away or replaced
  by a different file since it was registered, streaming reads of it fail (silence) until the
  instrument is reloaded.

- **`randomSeed(micros())` in the audio path.** `micros()` is frozen per audio block on the render thread (needed for exact MIDI sub-block timing), so a seed taken from it is deterministic per block rather than per call as on hardware.

## MIDI output

- `platforms/apple/hal/MidiCoreMidi.{h,cpp}` replaces the inert MIDI placeholders. It publishes the virtual source "Tracker CE (unofficial)" and can also send to one selected CoreMIDI destination (`listDestinations()`, `selectDestination(idx)`). MIDI input is not implemented.
- Timing: events produced during an audio block are stamped from the block index and sub-block offset through the latest Core Audio cycle mapping, plus the output latency and a user offset (-50..+50 ms, default 0). While a block renders, `TimerApple::micros()/millis()` on the render thread are frozen at the block's nominal start so `mtMidi.cpp` computes the exact sub-block delay. Events sent from the tracker thread (UI previews) are stamped now + output latency.
- **Converter fallback.** When the audio device is not at 44.1 kHz, `CoreAudioOutput` runs an AudioConverter whose own delay is not part of `outputLatencySeconds()`. MIDI timing is then late by that delay relative to audio (a message is logged once). Use the user offset to compensate.
- **Ordering.** The sender merges monotonically: tracker-thread events (UI previews, stop/panic) are stamped no earlier than the latest render-thread stamp already sent, so a note-off can never overtake a note-on rendered into a queued block.
- **Teardown order (Task 10).** Stop the `AudioRuntime` and the `CoreAudioOutput`, call `MidiCoreMidi::detachOutput(out)` (it clears the onCycle gate and never reassigns the `std::function`, so it is safe even when `stop()` timed out and an IOProc may still run), then destroy `MidiCoreMidi` (the sender thread holds a pointer to the output). On startup, call `attachOutput(out)` before `out.start()`: installing the onCycle `std::function` is not synchronised against the IOProc. On quit, if teardown is still running after 5 s the app flushes stdout/stderr and calls `_exit(0)` rather than letting `exit()` run static destructors under live threads.

## Building and running the Mac app

One command (CMake 3.20+, Ninja, Xcode with Swift; macOS 14+ deployment target):

```
cmake -S platforms/apple -B build-apple -G Ninja && ninja -C build-apple
open "build-apple/app/Tracker CE (unofficial).app"
```

This also builds the tests: `ctest --test-dir build-apple -LE hardware`. The audio-hardware tests (`coreaudio_tests`, label `hardware`; they need a real output device) are registered only with `-DTRACKERCE_HW_TESTS=ON`; run them with `ctest --test-dir build-apple -L hardware`. The app is a CMake-generated bundle (`local.trackerce.mac`, no icon); SwiftUI/AppKit live only under `platforms/apple/app/` (`Bridge.{h,mm}` is the Objective-C++ facade over the C++ port, Swift files are the UI). Debug builds (`-DCMAKE_BUILD_TYPE=Debug`) abort on any allocation on the audio render thread; set `TRACKERCE_ALLOC_GUARD=1` to get that in other builds.

Folder layout: the "SD card" is `~/Documents/Tracker CE/` (File > Choose SD Folder... changes it; takes effect on the next launch; stored in UserDefaults). The core creates the usual Tracker folders in it; put WAV samples in it as on a real card.

Settings (Cmd+,): audio device and left/right output channel, MIDI destination, MIDI timing offset (-50..+50 ms), SD folder. Device lists are read on a background queue; the audio status line (Opened / Fell back to / Failed + device) is shown there and in the debug overlay (Cmd+D, which also shows the underrun counter). The audio device persists by name; the MIDI destination persists by identity (CoreMIDI unique id, falling back to the display name), resolved against the live list when applied. The audio and MIDI lists refresh on Core Audio device-list / default-output and CoreMIDI setup-change notifications (off the main thread), and the saved MIDI choice is re-applied, so a replugged device reattaches.

Launch arguments for automation: `--screenshot PATH [--screenshot-after S]` (offscreen render of the current frame), `--debug`, `--quit-after S`. Test hooks (`--test-clock-play`: USB clock out on via the tracker thread, press Play after 3 s; `--test-key`; the `TRACKERCE_TEST_HANG_*` env vars) exist only when built with `-DTRACKERCE_TEST_HOOKS=ON` (the default for Debug builds, off otherwise), so a Release app does not ship them.

### Keyboard map (overlay: Cmd+/, also generated from `KeyMap.swift`)

Keys are matched by physical position (ANSI), so layout does not matter. Key repeat is ignored (a press is sent once; the core synthesises holds); losing window focus releases everything.

- Pads, rows of 12 keys: the first 8 keys of each row are grid row 1-4 (pads 0-31), the last 4 are key pads 32-47.
  - Row 1: `1 2 3 4 5 6 7 8 | 9 0 - =`
  - Row 2: `Q W E R T Y U I | O P [ ]`
  - Row 3: `A S D F G H J K | L ; ' \`
  - Row 4: `Z X C V B N M , | . /` then the grave-accent key and the key left of Z on ISO boards (pads 46, 47)
- Buttons: arrows = Up/Down/Left/Right, Space = Play, Esc = Rec, Return = Insert, Delete = Delete, Fwd-Delete = Copy, Tab = Shift (hold), F1-F12 = buttons 8-19 (Params ... Instr; needs the fn key on laptops).
- All 44 buttons: hold Control and press the pad key of the same index (Ctrl+1 = button 0, Ctrl+Q = button 8, Ctrl+A = button 16, ...; the Mini buttons 33-43 are Ctrl+ `0 - = O P [ ] L ; ' \\` (in that order), so +10/+1/-1/-10 = Ctrl+`]`, L, `;`, `'`).
- Encoder: Page Up / Page Down or keypad + / - or Option+. / Option+, = +1 / -1; add Shift for +-10. Trackpad or mouse scroll also turns it.

## Review Focus coverage

Plan Review Focus items and the tests that cover them (`ctest -LE hardware` unless marked):

| # | Review Focus | Tests |
|---|---|---|
| 1 | Device switching/loss mid-play | Hardware-free (`coreaudio_logic_tests`): `test_open_hang_returns_and_falls_back`, `test_open_with_wedged_previous_and_hanging_new_returns_in_bound`, `test_open_during_fallback_user_choice_wins`, `test_start_hang_falls_back_with_status`, `test_slow_first_cycle_does_not_fall_back` (stall watchdog), `test_abandoned_device_ioproc_never_pulls`, `test_destructor_during_fallback_runs_nothing_afterwards`. Hardware (`-L hardware`): `test_dead_device_default_still_dead_falls_to_builtin`, `test_failed_fallback_then_retry_on_device_change`, `test_stall_watchdog_reopens`, `test_external_rate_change_uses_converter`, `test_teardown_hang_*`. Manual M4. |
| 2 | Samples >= 256 KB via SD streaming | `test_virtual_sector_roundtrip`, `test_two_files_disjoint_sectors`, `test_fast_seek_idempotent_on_reopen`, `test_replaced_file_read_fails_cleanly` (`filesystem_tests`); manual M3. |
| 3 | Long main-loop work while playing | `test_render_continues_during_blocking_update` (`runtime_tests`). |
| 4 | Synth instruments in a project | `test_synth_project_roundtrip_identical` (`apple_tests`), `synth_stub_tests`. |
| 5 | Disabled "interrupts" | `test_no_block_inside_critical_section`, `test_tracker_suspended_during_block`, `test_global_irq_disable_from_render_thread_is_noop` (`runtime_tests`). |

The plan's `test_device_loss_falls_back` (item 1) was superseded: calling a private `handleDeviceDead()` and expecting a reopen turned out to need both the real fallback chain and the hang-proofing, so it was replaced by the injected-device-loss and hang tests above (`CoreAudioOutputTestHooks` fakes in `coreaudio_logic_tests`, and the `[hardware]` set in `coreaudio_tests`).

## Manual acceptance checklist (M1-M6, by ear / hands on)

Not automatable here (no audio hearing, no hardware). Launch the app, open the debug overlay (Cmd+D), keep `underruns` visible.

1. **M1** New project. Put 4 short WAVs (< 256 KB) and one long WAV (>= 1 MB) in the SD folder, load them (Sample Load is button 13: F6). Write an 8-step drum pattern, press Space. Steady drums for 5 minutes, no clicks, `underruns: 0`.
2. **M2** Settings: pick the ES-8 (left 1, right 2) and a MIDI destination (Semaphore or an IAC bus with a MIDI monitor). Add a MIDI track with notes on steps 1 and 3, turn MIDI clock out on (Config). Notes and clock arrive; with offset 0 roughly in time with the drums.
3. **M3** Trigger the long sample repeatedly while switching patterns. No glitches.
4. **M4** Unplug the ES-8 mid-play. Audio continues on the built-in output, the app stays responsive, the status line says "Fell back to: ...".
5. **M5** Save (File screen), quit (Cmd+Q), relaunch, load. Project identical; synth tracks still there, silent.
6. **M6** Open the Sample Recorder screen. It must not crash (input is silent).

Automated results are in `.superpowers/sdd/2026-11-05-apple-port-mvp/task-10-report.md`.

## Known gaps

- MIDI input is not implemented; Sample Recorder gets silent input.
- Synths are silent (stub) and patch edits are not saved (see Known limitations).
- IMX encoder acceleration is not mirrored: each key click or scroll step is exactly +-1 (x10 with Shift).
- MIDI timing tightening is pending (use the offset slider).
- Changing the SD folder needs a relaunch.

## Licence

Free/personal use. The port uses only Apple system frameworks and the vendored sources already in this repository (Catch2, fmt); no proprietary libraries. The app is unofficial: no Polyend trademarks or logos (no icon), name "Tracker CE (unofficial)". The core's own boot logo is shown in the window: that is fine for a free personal build, but replace it before any distribution (no trademark rights are granted).
