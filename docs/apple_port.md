# Apple port

## Core changes

Changes to `core/` made for the Apple host build. *neutral* means the hardware code is unchanged. Verification so far: `core/` objects only (`make -C core build` for MK66/ORIGINAL, IMX/PLUS, IMX/MINI) built on `main` and on this branch with a shimmed GCC 15.3 (the only runnable arm toolchain here), debug info stripped: 697 of 699 objects identical, the exception being `synth/Synth.o` as expected. The full `make original plus mini` build and `cmp` of the images with GCC 10.3 is still pending (needs Rosetta or the `agt` toolchain).

- `core: add PlatformTraits.h (platform::kIsApple)` - *neutral* (new header, unused on hardware).
- `core: host stubs for interrupt control and ELF section macros under PLATFORM_APPLE` (`config.h`, `cr_section_macros.h`) - *neutral*.
- `core: ordinary static storage for external RAM buffers on Apple (Mach-O sections)` (`Tracker.cpp`, `effect_polyverb.cpp`) - *neutral*.
- `core: skip mallinfo/malloc.h heap probing in Profiler on Apple` - *neutral*.
- `core: build synth patch paths with FileSystem::kPathSeparator` (`Synth.cpp`) - *neutral (string change)*: `\` becomes `/`, both accepted by FatFs. Changes `Synth.o`.
- `core: use uintptr_t for pointer-difference casts (64-bit hosts)` (`mtConfig.cpp`, `mtFileManagerOldVersionRecovery.cpp`, `mtAudioEngine.cpp`) - *neutral* (`uintptr_t` is 32 bits on Cortex-M).
- `core: const-correct strchr results in textBox (libc++ overloads)` - *neutral*.
- `core: use class-scope enumerators in fSampleRec case labels (clang requires constants)` - *neutral*.
- `core: portable C fallback for Polyverb Clip16 off ARM` - *neutral* (`#if defined(__arm__)` keeps the `ssat` asm).
- `core: portable C implementations of Cortex-M DSP intrinsics in dspinst.h for Apple` - *neutral* (hardware path untouched, new block under `PLATFORM_APPLE`).
- `core: keep strInstrument layout on 64-bit hosts via 4-byte SampleAddress handle` (`mtStructs.h`) - *neutral* (hardware keeps `int16_t*`).

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
