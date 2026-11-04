# Tracker CE Mac MVP Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** A macOS app that runs Tracker CE's unmodified `core/`. It renders the real 800×480 UI with Metal, takes keyboard input for pads/buttons/encoder, plays the sample engine through Core Audio, and sends latency-compensated MIDI through CoreMIDI.

**Architecture:**
- `core/` compiles for the host with clang as a static library, using the Tracker+ feature set (`PLATFORM_IMX`, `FEATURE_SYNTH`, PRODUCT=PLUS) plus a new `PLATFORM_APPLE` define.
- A new `platforms/apple/` provides:
  - every HAL interface from `core/include/Platform.h`
  - an FT812-modelling `DisplayDriver` that decodes display-list and coprocessor words into primitives for a Metal renderer
  - a silent stub for the proprietary PolySynth API
  - a thin SwiftUI/AppKit shell
- "Interrupt" semantics are emulated with two threads:
  - The tracker thread runs `update()`.
  - The render thread runs `updateAudioBuffers()` in 128-frame blocks while the tracker thread is suspended, so blocks never interleave with main-loop code, exactly as with an IRQ.
  - Core Audio only copies from a ring buffer.

**Tech Stack:** C++20 (clang), CMake + Ninja for the core and tests, Xcode project for the app, Swift 6 / SwiftUI, Metal, Core Audio (AUHAL), CoreMIDI, Catch2 (vendored at `util/3rdparty/catch2`).

**Spec:** `docs/superpowers/specs/2026-11-05-apple-port-mvp-design.md`

## Global Constraints

- Files under `core/interface/`, `core/libs/NewDisplay/` and `core/libs/FT812/` must stay unmodified.
- Every other core edit is either behind `#ifdef PLATFORM_APPLE` or a no-op on hardware.
- `make original`, `make plus` and `make mini` must still build, and produce identical binaries to `main` (`cmp` the outputs).
- Never link `synth/lib/libPolySynth.a` or `libPolyDSP.a`. Never disassemble or inspect them. Stubs are written only from the headers in `synth/include/PolySynth/`.
- Project files and instrument numbering must match Tracker+. That means `FEATURE_SYNTH` stays defined and `kTotalInstrumentsCount` is unchanged.
- Audio is 44,100 Hz with 128-frame blocks (`AUDIO_BLOCK_SAMPLES`) and int16 stereo, as on IMX.
- Shared code (everything except `platforms/apple/app/`) uses only Foundation, Metal, Core Audio, CoreMIDI, mach and POSIX. No AppKit, so the iPad build stays possible.
- No Polyend trademarks in the app name, bundle id or icon. App name: **"Tracker CE (unofficial)"**, bundle id `local.trackerce.mac`.
- The deployment target is macOS 14+. The arm64 Mac is the primary target.
- Synth compatibility is achieved by **linking a silent stub of the PolySynth API** (Task 2) with `FEATURE_SYNTH` left on. That way the hardware builds and the project layout don't change. This refines the spec's "split the flag" wording, with the same outcome.
- Out of scope: Sample Recorder (it receives silent input and must not crash; no core edits to its screen), MIDI input timing, `.ptfx` plugins, radio, battery, USB audio, firmware update, factory test, Tracker Mini.

## Review Focus

1. **Audio-device switching or loss mid-play.** Example: the ES-8 is unplugged, or the sample rate changes. Expected: the app keeps running, audio falls back to the default output, and there's no crash or hang. *Test: Task 7, `test_device_loss_falls_back`.*
2. **Samples ≥ 256 KB use the SD-streaming path** (`VoiceStreamManager`, sector reads). Expected: long samples play correctly with no clicks. *Test: Task 3, `test_virtual_sector_roundtrip`, and Task 10 manual check M3.*
3. **Long main-loop operations** (project load, sample import) while playing. Expected: audio keeps going with no dropouts, as on hardware. *Test: Task 6, `test_render_continues_during_blocking_update`.*
4. **Synth instruments in a project.** Expected: they load, save and display normally, play silently, and round-trip byte-identically. *Test: Task 4, `test_synth_project_roundtrip_identical`.*
5. **Disabled "interrupts"** (`DISABLE_AUDIO_INTERRUPTS`) during main-loop work. Expected: no audio block runs inside the critical section, and no deadlock. *Test: Task 6, `test_no_block_inside_critical_section`.*

---

## File Structure

```
platforms/apple/
  CMakeLists.txt                 # host build: libTrackerCore.a, libTrackerApple.a, tests
  config/config.h                # PLATFORM_APPLE versions of section macros (empty)
  core_compat/                   # shims for headers core expects (kinetis.h stub etc.)
  hal/                           # one file pair per HAL interface
    PlatformApple.{h,cpp}        # Platform impl; owns all HAL objects
    TimerApple.{h,cpp}           # mach_absolute_time; MIDI clock sub-block offset capture
    FileSystemPosix.{h,cpp}      # FileSystem on a root folder; virtual sector map
    VirtualSectorMap.{h,cpp}     # LBA <-> (file, offset) for fast-seek streaming
    InputApple.{h,cpp}           # SwitchGroup (pads, buttons), Encoder; event queue
    MidiCoreMidi.{h,cpp}         # UsbMidi + UartMidi → timestamped queue → CoreMIDI
    StubsApple.{h,cpp}           # Radio, Battery, Sgtl5000, Leds, Usb, UsbAudio, CRC, ConfigManager
    interrupts_apple.cpp         # enable/disableAudioInterrupt, sin_q15
  runtime/
    AudioRuntime.{h,cpp}         # tracker thread, render thread, suspend/resume, ring buffer
    CoreAudioOutput.{h,cpp}      # AUHAL output, device selection, latency query
  display/
    Ft812Model.{h,cpp}           # RAM_G/RAM_DL/CMD FIFO memory model (DisplayDriver impl)
    DlDecoder.{h,cpp}            # display-list word → primitive decoding (+ CMD_APPEND replay)
    CoproDecoder.{h,cpp}         # coprocessor commands (TEXT, NUMBER, GRADIENT, INFLATE, ...)
    Bitmaps.{h,cpp}              # EVE bitmap formats → RGBA8
    Frame.h                      # primitive list handed to the renderer
    MetalRenderer.{h,mm}         # draws a Frame; offscreen PNG mode for tests
  synth_stub/PolySynthStub.cpp   # silent definitions of PolySynth symbols core references
  app/                           # Xcode project + Swift shell (macOS only)
    TrackerCE.xcodeproj
    TrackerCEApp.swift, TrackerView.swift, KeyMap.swift, Settings.swift, Bridge.{h,mm}
  tests/                         # Catch2 tests, one file per component
docs/apple_port.md               # how to build/run, keyboard map, folder layout
```

---

### Task 1: Host build of `core/`

**Files:**
- Create: `platforms/apple/CMakeLists.txt`, `platforms/apple/config/config.h`, `platforms/apple/core_compat/kinetis.h`, `platforms/apple/tests/test_smoke.cpp`
- Modify (ifdef only): `core/config/config.h`, `core/libs/Audio/utility/pdb.h`, `core/libs/Audio/utility/dspinst.h` (only if the `KINETISK` guard is insufficient), plus any core file that fails to compile for a hardware-only reason.
- Exclude from the host build: `core/effector/ptfx_loader.cpp` (replaced by a stub in Task 2), `core/source/mtTest.cpp`.

**Interfaces:**
- Produces: CMake targets `TrackerCore` (static lib of all `core/` sources, same globs as `core/makefile`) and `apple_tests` (Catch2 runner). Compile definitions: `PLATFORM_APPLE PLATFORM_IMX FEATURE_SYNTH USB_MIDI LAYOUT_US_ENGLISH` plus `FW_VERSION_DEFINES` from `version.mk`. No `KINETISK`, no `__MK66FX1M0__`, no `TEENSYDUINO`. C++ flags: `-std=gnu++20 -fno-rtti -fno-exceptions -include cstdint`.

- [ ] **Step 1:** Add `test_smoke.cpp` with `TEST_CASE("core library links")`. It references `Tracker::getVersion()` and `REQUIRE(std::string(Tracker::getVersion()).size() > 0)`.
- [ ] **Step 2:** Run `cmake -S platforms/apple -B build-apple -G Ninja && cmake --build build-apple`. Expected: it FAILS with compile errors. Save the error list to the scratchpad as the work list.
- [ ] **Step 3:** Fix each error with the smallest ifdef or shim:
  - Under `PLATFORM_APPLE`, section macros (`AUDIO_CORE_RAM`, `AUDIO_CORE_FLASH`, `__NOINIT(...)`, `__SECTION*`) expand to nothing. Mach-O rejects ELF section names.
  - Inline ARM asm must use the existing C fallbacks.
  - Kinetis/Teensy includes resolve to `core_compat/`.
  - **Do not touch** `interface/`, `NewDisplay/` or `FT812/`. If one of them fails, fix it via `core_compat/` or `config.h` instead.
- [ ] **Step 4:** Build only the library for now. Linking `test_smoke` will fail on undefined platform symbols until Tasks 2–4, so mark it `EXCLUDE_FROM_ALL` until Task 4. Run `cmake --build build-apple --target TrackerCore`. Expected: the library builds with 0 errors. Record the warnings count; don't fix warnings.
- [ ] **Step 5:** Verify the hardware builds are unchanged. For `make original plus mini`, check out `main`, build, and copy the outputs to the scratchpad. Rebuild on this branch and `cmp` each `.ptf`/`.bin`. Expected: identical.
- [ ] **Step 6:** Commit: `build(apple): host CMake build of core as TrackerCore`.

### Task 2: PolySynth and ptfx stubs

**Files:**
- Create: `platforms/apple/synth_stub/PolySynthStub.cpp`, `platforms/apple/synth_stub/PtfxStub.cpp`, `platforms/apple/tests/test_synth_stub.cpp`

**Interfaces:**
- Consumes: `TrackerCore` (Task 1).
- Produces: CMake target `PolySynthStub`. It defines every symbol from `synth/include/PolySynth/*.h` that `TrackerCore` references.
  - Rendering produces silence (zero-filled buffers).
  - Patch and state serialization must preserve bytes. The stub must persist whatever patch blob the core hands it and return it unchanged, so projects round-trip. If a header's API makes byte-preservation impossible without the real lib, stop and report back. Don't guess.
  - `PtfxStub.cpp` makes plugin load return failure ("not supported on this platform").

- [ ] **Step 1:** Link a throwaway executable against `TrackerCore`, and collect the undefined symbols: `nm -u` / linker output, demangled, filtered to the PolySynth namespace(s) and ptfx_loader. Save the list to the scratchpad. Expected: a finite list. If it exceeds about 300 symbols, report the count before continuing.
- [ ] **Step 2:** Write `test_synth_stub.cpp`. `TEST_CASE("stub synth renders silence")`: render one block through the core's synth entry point and `REQUIRE` all samples == 0.
- [ ] **Step 3:** Run it. Expected: FAIL (unresolved symbols).
- [ ] **Step 4:** Implement the stubs from the headers only.
- [ ] **Step 5:** Run it. Expected: PASS.
- [ ] **Step 6:** Commit: `feat(apple): silent PolySynth and ptfx stubs`.

### Task 3: POSIX FileSystem with virtual sector streaming

**Files:**
- Create: `hal/FileSystemPosix.{h,cpp}`, `hal/VirtualSectorMap.{h,cpp}`, `tests/test_filesystem.cpp`

**Interfaces:**
- Produces:
  - `class FileSystemPosix : public FileSystem`. Its constructor takes `FileSystemPosix(std::string root)`. All core paths are absolute-from-card-root ("/Projects/…") and get mapped under `root`. `isSdCardInserted()` is true. Filters in `createFilesList` match the FatFs implementation in `platforms/imx/source/FileSystemImpl.cpp`, including hidden-file and extension rules, case-insensitive `.wav`/`.pti` matching, and sort order. Port the logic from it rather than re-deriving it.
  - `class VirtualSectorMap`:
    - `uint32_t registerFile(int fd, uint64_t size)` returns a base cluster.
    - `bool resolve(uint32_t lba, uint32_t count, int& fd, uint64_t& offset)`.
  - Constants: sector 512 bytes, `kSectorsPerCluster = 64`, `kDatabaseSector = 2048`.
  - `enableFastSeek(fh, clmt, n, outDb, outCsize)` registers the file as one contiguous fragment. It writes the FatFs CLMT format `[clmt[0] = needed size, clusterCount, startCluster, 0]` and sets `*outDb = kDatabaseSector` and `*outCsize = kSectorsPerCluster`. It returns true.
  - `readSectorsAsync(buf, lba, count)` resolves through the map and `pread`s synchronously. Then `pollAsyncRead` returns done/success, `waitAsyncRead` returns true and `isAsyncReadBusy` returns false.
  - LBA mapping, matching FatFs: `lba = db + (cluster − 2) × csize + sectorInCluster`. Each registered file gets a disjoint cluster range, starting at cluster 2 and advancing by `ceil(size / (csize × 512)) + 1`.
  - `nesDir_*` and `readNbFiles` support only what the core needs to not crash. Game ROMs are out of scope, so they may return empty.

- [ ] **Step 1:** Write the tests:
  - `test_open_read_write_seek`
  - `test_mkdir_rename_remove_dirs`
  - `test_create_files_list_wave_filter`: folder with `a.wav`, `B.WAV`, `c.txt`, `.hidden.wav` and subfolder `d/`. Expect `[d, a.wav, B.WAV]` in the IMX order.
  - `test_virtual_sector_roundtrip`, Review Focus 2: write a 1 MB file of a known pattern and call `enableFastSeek`. Compute the LBA for byte offset 300,000 with the FatFs formula, using the returned clmt/db/csize. `readSectorsAsync` 4 sectors, then `REQUIRE` the bytes match the pattern at `floor(300000/512) × 512`.
  - `test_two_files_disjoint_sectors`.
- [ ] **Step 2:** Run. Expected: FAIL.
- [ ] **Step 3:** Implement.
- [ ] **Step 4:** Run. Expected: PASS.
- [ ] **Step 5:** Commit: `feat(apple): POSIX FileSystem with virtual sector streaming`.

### Task 4: Platform HAL skeleton and headless boot

**Files:**
- Create: `hal/PlatformApple.{h,cpp}`, `hal/TimerApple.{h,cpp}`, `hal/InputApple.{h,cpp}`, `hal/StubsApple.{h,cpp}`, `hal/interrupts_apple.cpp`, `display/Ft812Model.{h,cpp}` (minimal: accepts all writes, always reports the coprocessor as empty), `tests/test_boot.cpp`

**Interfaces:**
- Consumes: `TrackerCore`, `PolySynthStub`, `FileSystemPosix` (Task 3).
- Produces:
  - `class PlatformApple : public Platform`. Its constructor takes `PlatformApple(const std::string& sdRootPath)`; its getters return the owned HAL objects.
  - `class TimerApple : public Timer`. `millis()` and `micros()` come from `mach_absolute_time` since construction. `clockCyclesPerUs()` returns 600. `scheduleMidiClockUs(delayUs, cb)` calls `cb` immediately, with `TimerApple::currentSubBlockOffsetUs()` returning `delayUs` during the call (thread-local; read by Task 8).
  - `class InputApple`. It provides two `SwitchGroup` instances (`pads()` with 48 indices, `buttons()` with 44 indices matching `interfaceButton0..43`) and an `Encoder`.
    - Thread-safe producer: `void post(InputEvent e)`, where `struct InputEvent { enum Kind { Pad, Button, Encoder } kind; uint8_t index; bool down; int delta; }`.
    - Consumer: `void dispatch()`, called from the tracker thread's loop. It fires the registered press/release/hold callbacks and synthesizes holds after `setHoldTime` ms.
    - Encoder delta goes to the core through whatever the IMX platform calls for encoder movement. Mirror `platforms/imx/source/EncoderImpl.cpp`.
  - `ConfigManager` is file-backed (`<sdRoot>/.config.bin`, zero-filled on first run). CRC is a real CRC-32 matching `platforms/imx/source/CrcCalculatorImpl.cpp`'s polynomial. Radio, Battery, Sgtl5000, Leds, Usb and UsbAudio are inert. `Battery::getLevel()` returns 100. `getState()` returns false (no battery).
  - `enableAudioInterrupt`/`disableAudioInterrupt` forward to `AudioRuntime` (Task 6). Until then, they are a nesting counter.
  - `sin_q15` is a C implementation with the same range and semantics as CMSIS `arm_sin_q15`.

- [ ] **Step 1:** Write `test_boot.cpp`:
  - `TEST_CASE("tracker boots headless")`: make a temp SD root, construct `PlatformApple`, call `Tracker::getInstance()->init(&p)`, then call `update()` 500 times and `updateAudioBuffers(l, r, 128)` 100 times. `REQUIRE` no crash and that `l`/`r` are written.
  - `TEST_CASE("sin_q15 matches reference")`: check 0 → 0, 0x2000 → 32767 ±1, 0x6000 → −32768 ±1.
  - `TEST_CASE("pad press reaches core")`: register a callback on `pads()`, `post` Pad 5 down, `dispatch()`, and `REQUIRE` the callback got 5.
  - `TEST_CASE("test_synth_project_roundtrip_identical")`, Review Focus 4: create a project, assign a synth instrument, save, load and save again. `REQUIRE` the two project files are byte-identical.
- [ ] **Step 2:** Run `ctest --test-dir build-apple -R boot`. Expected: FAIL (classes undefined).
- [ ] **Step 3:** Implement the classes listed in Interfaces. Use `FileSystemPosix` from Task 3. Boot needs the SD folder layout to exist: create the folders the core expects on a fresh card (find them via `mkdir` calls in `core/FileManager`).
- [ ] **Step 4:** Run the tests. Expected: PASS.
- [ ] **Step 5:** Commit: `feat(apple): HAL skeleton, headless boot test`.

### Task 5: FT812 model and display-list decoder

**Files:**
- Modify: `display/Ft812Model.{h,cpp}` (full model)
- Create: `display/DlDecoder.{h,cpp}`, `display/CoproDecoder.{h,cpp}`, `display/Bitmaps.{h,cpp}`, `display/Frame.h`, `tests/test_display_decode.cpp`, `tests/data/` (captured word streams)

**Interfaces:**
- Consumes: the opcode encodings in `core/libs/FT812/FT81x.h` and `FT812.cpp`. Reuse the same macros in tests to encode words.
- Produces:
  - `Frame.h`: `struct Prim { enum Kind { Points, Lines, LineStrip, EdgeStrip*, Rects, Bitmap, Gradient } kind; ... }`, carrying colour (RGBA8), line width / point size in 1/16 px, a scissor rect, the blend mode, vertices in 1/16 px and, for bitmaps, a texture id plus source rect. Also `struct Frame { uint32_t clearColor; std::vector<Prim> prims; uint64_t serial; }`.
  - `class Ft812Model : public DisplayDriver`. It holds `ram_g` (1 MB, `RAM_G_SIZE`), `ram_dl` (8 KB) and a coprocessor FIFO.
    - `WriteData32` and `IncCMDOffset` append to the current coprocessor list.
    - `endCoProList()` executes the list via `CoproDecoder`. `isCoProEmpty()` returns 1 and `awaitCoProEmpty()` returns immediately.
    - `writeDataRAMG`, `writeFontRAMG`, `readDataRAMG` and `readBackDataRAMG` copy to and from `ram_g`.
    - The stream DMA trio copies `streamBuf` into `ram_g` synchronously.
    - On `CMD_SWAP` (or `DL_SWAP`), `DlDecoder` decodes `ram_dl` into a `Frame` and publishes it: `std::shared_ptr<const Frame> latestFrame()`, which the renderer reads lock-free via an atomic shared_ptr swap.
  - `CoproDecoder` covers `CMD_DLSTART`, `CMD_SWAP`, `CMD_APPEND` (copy from `ram_g` into the DL), `CMD_MEMCPY`, `CMD_MEMSET`, `CMD_MEMWRITE`, `CMD_INFLATE` (zlib via `libcompression` or `zlib`), `CMD_SETBITMAP`, `CMD_TEXT`, `CMD_NUMBER`, `CMD_GRADIENT`, `CMD_SNAPSHOT2`, `CMD_TRANSLATE`/`CMD_SCALE`/`CMD_SETMATRIX`/`CMD_LOADIDENTITY`, `CMD_SETFONT2`/`CMD_ROMFONT` (whichever the code uses), and anything else found in Step 1.
    - `CMD_TEXT` and `CMD_NUMBER` expand to Bitmap prims using the font metric block in `ram_g`, exactly as EVE does. The font format follows the EVE programmer guide's "font metric block".
    - `CMD_SNAPSHOT2` stores the last published Frame's pixels into `ram_g` at the given address and format, by asking the renderer through a callback `std::function<bool(Rect, uint32_t addr, uint32_t fmt)> snapshotHook`. In headless tests it fills black.
  - `Bitmaps::decode(fmt, const uint8_t* src, w, h, stride, palette) -> std::vector<uint32_t>` supports L1, L2, L4, L8, ARGB1555, ARGB4, RGB565, RGB332, ARGB2 and PALETTED variants, or the subset Step 1 finds.

- [ ] **Step 1: Inventory.** Grep `core/libs/NewDisplay`, `core/libs/FT812` and `core/source` for every `API_` / `CMD_` / bitmap-format constant used. Write the list to `platforms/apple/display/INVENTORY.md`. Anything this Task's Interfaces don't cover gets added to the decoder scope.
- [ ] **Step 2:** Write the tests:
  - `test_points_and_lines`: encode `CLEAR_COLOR_RGB, CLEAR, BEGIN(LINES), COLOR_RGB(255,0,0), LINE_WIDTH(32), VERTEX2F(160,160), VERTEX2F(320,160), END, DISPLAY` into `ram_dl` via the API path. `REQUIRE` one Lines prim, red, width 32/16 = 2 px, vertices (10,10)→(20,10).
  - `test_save_restore_context`
  - `test_scissor`
  - `test_append_replays_cached_list`: write a DL snippet to `ram_g` with `CMD_MEMCPY` and then `CMD_APPEND` it twice. `REQUIRE` the prims appear twice.
  - `test_text_uses_font_metrics`: load `LiberationSans-Regular_14_L4` via `writeFontRAMG` and `CMD_SETFONT2`, then `CMD_TEXT(0,0,font,0,"Ab")`. `REQUIRE` 2 Bitmap prims whose x-advance matches the metric block's widths for 'A'.
  - `test_inflate`: zlib-compress 4 KB, `CMD_INFLATE` it to address X, and `REQUIRE` `ram_g` matches.
  - `test_l4_decode`: 2×1 L4 bytes `0xF0` decode to alpha 255, 0.
- [ ] **Step 3:** Run. Expected: FAIL.
- [ ] **Step 4:** Implement.
- [ ] **Step 5:** Run. Expected: PASS.
- [ ] **Step 6:** Extend `test_boot.cpp` with `TEST_CASE("boot produces frames")`: after 500 `update()` calls, `REQUIRE(latestFrame() && latestFrame()->prims.size() > 50)`.
- [ ] **Step 7:** Commit: `feat(apple): FT812 model and display-list decoder`.

### Task 6: Interrupt-faithful runtime (tracker and render threads)

**Files:**
- Create: `runtime/AudioRuntime.{h,cpp}`, `tests/test_runtime.cpp`
- Modify: `hal/interrupts_apple.cpp`, so it forwards to `AudioRuntime`

**Interfaces:**
- Consumes: `Tracker`, `PlatformApple`, `InputApple::dispatch()`.
- Produces: `class AudioRuntime`. Its constructor takes `AudioRuntime(Tracker*, PlatformApple*, size_t ringBlocks = 4)`.
  - `start()` spawns the tracker thread and the render thread. `stop()` ends them.
  - `size_t pull(float* l, float* r, size_t frames)` is real-time safe, with no locks or allocation. It copies from the SPSC ring and returns the frames delivered, zero-filling on underrun.
  - `uint64_t blockIndexAt(size_t ringReadPos)` maps ring positions to block indices.
  - `std::atomic<uint64_t> underruns`.
  - `void disableAudioInterrupt()` and `void enableAudioInterrupt()` are nesting-counted and called from the tracker thread.
  - Hook for Task 8: `std::function<void(uint64_t blockIndex)> onBlockRendered`.

Algorithm, which the tests don't fully pin:
```
tracker thread:  tracker->init(); loop { input->dispatch(); tracker->update(); sleep_for(500us) }
                 (init/update run with the "main" role; store its mach thread port)
render thread:   (real-time QoS / os_workgroup joined to the AUHAL workgroup when available)
  loop:
    wait until ring has space for one 128-frame block (semaphore signalled by pull())
    if irqDisableDepth > 0: set pendingIrq = true; wait on irqEnabledSem; continue
    thread_suspend(trackerPort)
      re-check irqDisableDepth (race with a disable that landed before suspend):
        if > 0 → thread_resume, go back to wait on irqEnabledSem
    tracker->updateInputAudioBuffers(zeros, zeros, 128)   // as hardware does every block; recorder input is out of scope
    tracker->updateAudioBuffers(int16 L[128], R[128], 128)
    thread_resume(trackerPort)
    convert int16 → float, push to ring; onBlockRendered(blockIndex++)
enableAudioInterrupt(): --depth; if 0 and pendingIrq → signal irqEnabledSem
```
Code that runs inside `updateAudioBuffers` must not take locks that the tracker thread might hold. The MIDI sink in Task 8 is a lock-free queue for this reason. Logging from the audio path goes to a lock-free ring, never `printf`.

- [ ] **Step 1:** Write the tests, using a fake `Tracker` that records thread IDs and timestamps:
  - `test_tracker_suspended_during_block`: the fake `update()` increments an atomic counter in a tight loop. The fake `updateAudioBuffers` reads the counter at entry, spins 100 µs, then reads it again. `REQUIRE` the two reads are equal over 1,000 blocks, which proves the tracker thread is frozen during a block.
  - `test_no_block_inside_critical_section` (Review Focus 5): run 10,000 disable/enable pairs in `update()` with a 50 µs busy section, and `REQUIRE` zero blocks rendered while the depth is > 0.
  - `test_render_continues_during_blocking_update` (Review Focus 3): the fake `update()` sleeps 200 ms once. Pull continuously at the real-time rate for 1 s, and `REQUIRE(underruns == 0)`.
  - `test_pull_is_zero_filled_on_underrun`.
- [ ] **Step 2:** Run. Expected: FAIL.
- [ ] **Step 3:** Implement.
- [ ] **Step 4:** Run 20× (`ctest --repeat until-fail:20 -R runtime`). Expected: PASS every time.
- [ ] **Step 5:** Commit: `feat(apple): interrupt-faithful audio runtime`.

### Task 7: Core Audio output

**Files:**
- Create: `runtime/CoreAudioOutput.{h,cpp}`, `tests/test_coreaudio.cpp`

**Interfaces:**
- Consumes: `AudioRuntime::pull`.
- Produces: `class CoreAudioOutput`.
  - `static std::vector<DeviceInfo> listOutputDevices()`, with `struct DeviceInfo { AudioDeviceID id; std::string name; uint32_t outChannels; }`.
  - `bool open(AudioDeviceID id, uint8_t leftCh = 0, uint8_t rightCh = 1)`, then `start()` and `stop()`.
  - `double outputLatencySeconds() const` = device latency + safety offset + stream latency + buffer frame size, all ÷ the nominal rate.
  - `std::function<void(uint64_t hostTimeOfFirstFrame, uint64_t blockIndex, size_t offsetInBlock)> onCycle` fires per IOProc cycle so Task 8 can map block indices to host time.
  - Behaviour:
    - Request a nominal 44,100 Hz. If the device refuses, open at its rate and insert an `AudioConverter` (fallback path).
    - Listen for `kAudioDevicePropertyDeviceIsAlive` and the default-device change. When the device is lost, reopen the default output without stopping the runtime.

- [ ] **Step 1:** Write the tests, tagged `[hardware]` and skipped in CI. `test_lists_builtin_output`, and `test_device_loss_falls_back` (Review Focus 1). For the latter, simulate by calling the private `handleDeviceDead()` and `REQUIRE` it reopens the default device and that `pull` is still called within 500 ms.
- [ ] **Step 2:** Run. Expected: FAIL.
- [ ] **Step 3:** Implement with an AUHAL output unit. The render callback calls only `pull` and deinterleaves into the selected channels; all other channels are zeroed. The ES-8 exposes 8 outputs.
- [ ] **Step 4:** Run. Expected: PASS. Then a manual check: a CLI `trackerce-headless --tone` (a test tool in this task) plays a 440 Hz test tone through the default output and through the ES-8 if connected.
- [ ] **Step 5:** Commit: `feat(apple): Core Audio output with device fallback`.

### Task 8: CoreMIDI output with latency-compensated timestamps

**Files:**
- Create: `hal/MidiCoreMidi.{h,cpp}`, `tests/test_midi_timing.cpp`

**Interfaces:**
- Consumes: `AudioRuntime::onBlockRendered`, `CoreAudioOutput::onCycle`/`outputLatencySeconds`, and `TimerApple::currentSubBlockOffsetUs()`.
- Produces:
  - `class MidiCoreMidi`, which implements both `UsbMidi` and `UartMidi` (two instances, both feeding one sender).
    - Every `send*` call made on the render thread enqueues `{blockIndex, offsetUs, bytes[3]}` onto an SPSC queue, with no locks. `offsetUs` is `TimerApple::currentSubBlockOffsetUs()` when inside a `scheduleMidiClockUs` callback, else 0.
    - Calls made on the tracker thread (UI previews) are stamped "now".
  - A sender thread converts block index to host time using the latest `onCycle` mapping: `hostTime(block) = hostTimeOfFirstFrame + ((block − cycleBlock) × 128 − offsetInBlock) / 44100 s`. It then adds `outputLatencySeconds()` + `offsetUs` + the user offset (ms, `int userOffsetMs` in −50..+50, default 0) and calls `MIDISend`/`MIDISendEventList` with that timestamp.
  - `static std::vector<std::string> listDestinations()`, `bool selectDestination(int idx)`, and a virtual source "Tracker CE (unofficial)" that's always published.
  - Read and receive functions are inert (MIDI input is out of scope): `isReadAvailable()` returns false.

- [ ] **Step 1:** Write the tests with a fake clock mapping:
  - `test_timestamp_math`: cycle mapping block 10 → host t0, offsetInBlock 0, latency 5 ms. An event at block 12 with offset 1000 µs must get t0 + 256/44100 s + 5 ms + 1 ms (exact, in host ticks via `mach_timebase_info`).
  - `test_user_offset_applied`
  - `test_render_thread_send_does_not_lock`: the enqueue path makes no allocation. Check with a counting `operator new` override in the test binary.
- [ ] **Step 2:** Run. Expected: FAIL.
- [ ] **Step 3:** Implement.
- [ ] **Step 4:** Run. Expected: PASS.
- [ ] **Step 5:** Commit: `feat(apple): CoreMIDI output with latency-compensated timestamps`.

### Task 9: Metal renderer

**Files:**
- Create: `display/MetalRenderer.{h,mm}`, `display/Shaders.metal`, `tests/test_render_golden.mm`, `tests/golden/*.png`

**Interfaces:**
- Consumes: `Frame` and `Ft812Model::latestFrame()` / `snapshotHook`.
- Produces:
  - `class MetalRenderer`. Its constructor takes `MetalRenderer(id<MTLDevice>)`.
    - `void draw(const Frame&, id<MTLRenderCommandEncoder>, CGSize drawablePx)` draws in an 800×480 logical space scaled to the drawable. Scaling is integer when it fits, otherwise uniform with letterbox.
    - `bool renderToPNG(const Frame&, const char* path, int scale = 1)` is the offscreen path for tests.
    - `void uploadBitmaps(const Ft812Model&)` caches textures keyed by (ram_g addr, fmt, w, h, generation counter bumped on writes to that range).
  - Primitive semantics follow the EVE programmer guide:
    - POINTS are discs, with radius = point size.
    - LINES are capsules (round caps), with width = line width.
    - RECTS are rounded rects with corner radius = line width.
    - Edge strips, bitmaps with cells, and `BLEND_FUNC` (SRC_ALPHA/ONE_MINUS_SRC_ALPHA default, ONE/ZERO, etc.) are supported. Scissor uses `setScissorRect`.
    - Anti-alias with an SDF in the fragment shader, about 1 px of feathering at 1× scale.

- [ ] **Step 1:** Write the golden tests:
  - Render three synthetic Frames (lines+points, rounded rects+scissor, text via the font from Task 5) to PNG and compare with `tests/golden/*.png` at a ≤ 1% pixel-difference tolerance.
  - On first run, golden files don't exist. The test writes them, FAILS, and prints the paths. The implementer inspects them visually, then commits them.
- [ ] **Step 2:** Run. Expected: FAIL (no renderer).
- [ ] **Step 3:** Implement.
- [ ] **Step 4:** Run. Expected: PASS against the reviewed goldens.
- [ ] **Step 5:** Add `trackerce-headless --screenshot out.png --updates 500`, which boots the real core and renders the last frame. Run it, open the PNG, and compare by eye with a photo of a real Tracker+ start/pattern screen; ask the user for one if needed. Note the visible differences in `INVENTORY.md`.
- [ ] **Step 6:** Commit: `feat(apple): Metal renderer with golden tests`.

### Task 10: Mac app shell, keyboard map, settings, docs

**Files:**
- Create: `platforms/apple/app/TrackerCE.xcodeproj`, `TrackerCEApp.swift`, `TrackerView.swift` (MTKView, 60 Hz, draws `latestFrame()`), `KeyMap.swift`, `Settings.swift`, `Bridge.{h,mm}` (Objective-C++ façade over `PlatformApple`, `AudioRuntime`, `CoreAudioOutput`, `MidiCoreMidi` and `MetalRenderer`), `docs/apple_port.md`

**Interfaces:**
- Consumes: everything above.
- Bridge (Objective-C):
  - `-(instancetype)initWithSDRoot:(NSString*)path`
  - `-(void)start`, `-(void)stop`
  - `-(void)postPad:(int)i down:(BOOL)d`, `-(void)postButton:(int)i down:(BOOL)d`, `-(void)postEncoder:(int)delta`
  - `-(NSArray<NSString*>*)audioDevices`, `-(BOOL)selectAudioDevice:(NSInteger)i left:(int)l right:(int)r`
  - `-(NSArray<NSString*>*)midiDestinations`, `-(BOOL)selectMidiDestination:(NSInteger)i`
  - `@property int midiOffsetMs`
  - `-(void)drawInEncoder:(id<MTLRenderCommandEncoder>)e size:(CGSize)s`
- Behaviour:
  - The SD root defaults to `~/Documents/Tracker CE/`, with a "Choose Folder…" menu item. The choice persists in `UserDefaults`.
  - The Settings window (⌘,) has the audio device and L/R channel pickers, the MIDI destination picker and the MIDI offset slider (−50…+50 ms).
  - Keyboard: every pad (48), every button (`interfaceButton0..43`) and the encoder (±1, ±10 with Shift) must be reachable.
    - Suggested layout: pads on Q-row/A-row/Z-row/number-row (12 per row, using `1234567890-=`, `qwertyuiop[]`, `asdfghjkl;'\`, `zxcvbnm,./` + right-shift-side keys), buttons on the function keys and arrow keys with modifier layers, and the encoder on the trackpad scroll or `,`/`.` with Option.
    - The final map is the implementer's call, documented in `docs/apple_port.md` and shown by an in-app overlay (hold `?` / ⌘/).
    - Key repeat must not re-send presses. Track key-down state.

- [ ] **Step 1:** Build and run from Xcode. Expected: the window shows the Tracker start screen, then the pattern editor.
- [ ] **Step 2: Manual acceptance (the spec's success scenario).** Record the results in the PR description.
  - **M1:** Create a new project. Load 4 WAV drum samples (< 256 KB) from the SD folder and one long sample (≥ 1 MB). Write an 8-step drum pattern and press play. Expected: steady drums from the Mac output with no clicks over 5 minutes, and `underruns == 0` (shown in a debug overlay).
  - **M2:** Select the ES-8 (outputs 1/2) and a MIDI destination (Semaphore via USB, or any IAC bus with a DAW/MIDI monitor). Add a MIDI track with notes on 1 and 3 and turn on MIDI clock out. Expected: notes and clock arrive. With the offset at 0, notes sound roughly in time with the drums by ear (tightening comes later, per the spec).
  - **M3:** Trigger the long sample repeatedly while switching patterns. Expected: no glitches (exercises the streaming path).
  - **M4:** Unplug the ES-8 mid-play. Expected: audio continues on the built-in output and the app stays responsive.
  - **M5:** Save, quit, relaunch and load. Expected: the project is identical, and synth tracks (if any were added) remain, silent.
  - **M6:** Open the Sample Recorder screen. Expected: it doesn't crash; it shows its stubbed/unsupported state.
- [ ] **Step 3:** Write `docs/apple_port.md`: build steps (`cmake` and Xcode), the folder layout, the keyboard map, known gaps (MIDI in, recorder, synths silent, timing tightening pending) and the licence note (free/personal, no proprietary libs).
- [ ] **Step 4:** Run the full test suite: `ctest --test-dir build-apple -LE hardware`. Expected: all pass. Run the hardware-unchanged check from Task 1, Step 5 again. Expected: identical.
- [ ] **Step 5:** Commit: `feat(apple): macOS app shell, keyboard map, settings, docs`.
