# FT812 usage inventory (living document)

What the core asks of the FT812, and how the Mac model (`Ft812Model` + `CoproDecoder` + `DlDecoder`)
handles it. Counts are call sites in `core/` outside `core/libs/FT812/` (grep of `API_*`, Task 5a).
Task 5b extends this (text, bitmaps, gradients, matrices, image loading).

## How the core drives the chip

- Widget update (`display.cpp:484-565`): `CMD_DLSTART`, widget's display-list words and coprocessor
  widgets, end of list. Then `getDisplayListOffset()` (REG_CMD_DL) and
  `CMD_MEMCPY(150000 + i*8192, RAM_DL, off)` caches the **expanded** list in RAM_G
  (`displayStructs.h:22-23`). Multi-stage widgets append further stages after `ramSize`.
- Frame (`display.cpp:621-666`): `CMD_DLSTART`, `CLEAR_COLOR`, `CLEAR(1,1,1)`, `VERTEX_FORMAT(0)`,
  one `CMD_APPEND` per visible widget (reverse creation order), `DISPLAY`, `CMD_SWAP`.
- `arrangement.cpp:188-232` does the same per refresh step, wrapped in `SAVE/RESTORE_CONTEXT`.
- Boot logo (`display.cpp:294-345`) and font upload (`display.cpp:227-291`) are direct
  DLSTART..SWAP lists.

- The UI is wall-clock driven: the start screen (logo bitmap + progress rect, 2 prims) stays up
  for at least ~2 s (`interfaceMisc.cpp` `refreshStartScreen`), so headless runs pace `update()`
  (`trackerce-headless --interval-ms`, default 10). The pattern editor that follows is ~870 prims.

The model therefore executes every coprocessor command into `ram_dl` at `dlOffset` exactly like EVE;
only `CMD_SWAP` decodes `ram_dl` (up to `DISPLAY`) into a `Frame`.

## Display-list instructions

| Instruction | Uses | 5a status |
|---|---|---|
| `VERTEX2F` | 510 | decoded; scaled by `VERTEX_FORMAT` (default 4 = 1/16 px), plus `VERTEX_TRANSLATE` |
| `COLOR` / `COLOR_RGB` | 251 | context |
| `BEGIN` | 172 | RECTS 70, BITMAPS 37, LINES 31, LINE_STRIP 31, FTPOINTS 3 (no edge strips, decoded anyway) |
| `END` | 164 | closes the primitive |
| `LINE_WIDTH` | 123 | context |
| `SAVE_CONTEXT` / `RESTORE_CONTEXT` | 60 / 60 | 4-deep stack (nesting depth 2 in `arrangement.cpp`) |
| `BLEND_FUNC` | 58 | (SRC_ALPHA, ONE_MINUS_SRC_ALPHA) 29, (SRC_ALPHA, ZERO) 25, (SRC_ALPHA, ONE) 4 |
| `BITMAP_HANDLE` | 31 | context (handles 0 = boot logo, 1..5 = fonts) |
| `SCISSOR_XY` / `SCISSOR_SIZE` | 28 / 28 | context |
| `CELL` | 19 | context |
| `BITMAP_SIZE` | 16 | per-handle table |
| `CLEAR_COLOR` / `CLEAR` | 15 / 15 | all at the start of a list |
| `BITMAP_SOURCE` / `BITMAP_LAYOUT` | 13 / 13 | per-handle table |
| `VERTEX2II` | 11 | decoded (font glyphs, spectrum) |
| `DISPLAY` | 10 | ends decoding |
| `VERTEX_FORMAT` | 5 | always `VERTEX_FORMAT(0)` in the core |
| `BITMAP_TRANSFORM_A` / `_E` | 4 / 2 | context (8.8 fixed, 256 = 1.0): stream 128 (2x), `img.cpp:119` 276 |
| `POINT_SIZE` | 3 | context |
| `COLOR_A` | 1 | context (`label.cpp` shadow) |
| `BITMAP_LAYOUT_H` / `BITMAP_SIZE_H` | 1 / 1 | per-handle high bits (image viewer) |

Never used by the core but decoded for completeness: `ALPHA_FUNC`, `STENCIL_*`, `CLEAR_STENCIL`,
`CLEAR_TAG`, `TAG`, `TAG_MASK`, `COLOR_MASK`, `PALETTE_SOURCE`, `VERTEX_TRANSLATE_X/Y`,
`BITMAP_TRANSFORM_B/C/D/F`, `CALL`/`JUMP`/`RETURN`, `NOP`. `MACRO` is ignored (logged once).
Stencil, alpha test, colour mask and tag do not reach `Frame`.

## Coprocessor commands

| Command | Uses | Where | 5a status |
|---|---|---|---|
| `CMD_TEXT` | 58 | 21 files | **5b**: skipped by length (header + NUL-terminated padded string) |
| `CMD_GRADIENT` | 26 | bars, lists, tracker shadows | **5b**: skipped (20 bytes) |
| `CMD_DLSTART` | 13 | | `dlOffset = 0` |
| `CMD_SWAP` | 8 | | decode `ram_dl`, publish `Frame` |
| `CMD_NUMBER` | 8 | `songPlayer.cpp` | **5b**: skipped (16 bytes) |
| `CMD_APPEND` | 8 | | copy RAM_G -> `ram_dl[dlOffset]`, advance |
| `CMD_SETBITMAP` | 6 (3 live) | `label.cpp:330`, `battery.cpp:120`, `img.cpp:111`, `stream.cpp:120` | **5b**: skipped (16 bytes) |
| `CMD_MEMCPY` | 6 | widget cache | implemented (RAM_G and RAM_DL address ranges) |
| `CMD_TRANSLATE` / `CMD_ROTATE` / `CMD_LOADIDENTITY` / `CMD_SETMATRIX` | 4 / 2 / 2 / 2 | rotated text `label.cpp:209-213`, `songPlayer.cpp:643-647` | **5b**: skipped |
| `CMD_LOADIMAGE` + `CMD_GETPROPS` | 1 + 1 | image viewer `display.cpp:419,456-460` | **5b**: skipped; LOADIMAGE swallows the following JPEG/PNG bytes (until FFD9 / IEND) so the stream stays in sync |
| `CMD_SNAPSHOT2` | 2 | screenshots `snapshots.cpp:85,100` (fmt 0x20 ARGB8) | skipped (later task) |
| `CMD_MEMZERO` | 1 | `snapshots.cpp:117` | implemented |
| `CMD_SETFONT2` | 2 | font upload `display.cpp:286,1369` | **5b**: skipped (16 bytes) |
| `CMD_SETBASE` | 2 | `songPlayer.cpp:240,244` (hex numbers) | **5b**: skipped (8 bytes) |
| `CMD_MEMSET` / `CMD_MEMWRITE` | 0 | | implemented (brief) |

No `CMD_INFLATE` (fonts and images are RLE-decoded on the CPU, `display.cpp:240,265,309`).
`mtTest.cpp` (factory test) also uses `CMD_TEXT` with ROM font 28 and `CMD_GRADIENT`.
Unknown or not-yet-implemented commands are logged once per opcode and skipped by the byte length
the core's encoder (`FT812.cpp`) gives them.

`CMD_TEXT` options used: `OPT_CENTERY` 56, `OPT_CENTERX` 35, `OPT_RIGHTX` 33, `OPT_CENTER` 18.

## Bitmap formats

- From the core's tables: L4 (26 uses: fonts, icons), L2 (16: synth logos), L8 (2: boot logo).
- RGB565: image viewer (`img.cpp:111`, after `CMD_LOADIMAGE`) and NES video stream (`stream.cpp`,
  format 7 when `byte_res == 2`, else 4 = RGB332).
- PALETTED4444: only in a commented-out line (`img.cpp:105`); would need `PALETTE_SOURCE`.
- In 5a, `Bitmap` prims are decoded fully into `Frame` (source, format, size, stride, cell, palette,
  transform A-F) and `CpuRaster` draws them as placeholder outlines.

## Driver-level calls

- `writeDataRAMG` (bitmaps at boot), `writeFontRAMG` (fonts: bytes 144..147 of the 148-byte metric
  block are replaced with `dest + 148`, as `API_LIB_WriteFontRAMG` does), `readDataRAMG`
  (screenshots), `readBackDataRAMG` (unused; byte-reversed read starting at `src - 1`, mirrored).
- `writeDataToCMD` (image viewer) appends to the command stream and executes it at once, like the IMX
  driver which bumps REG_CMD_WRITE itself.
- `sendString` mirrors `API_SendString` (length 0 = strlen, NUL, pad to 4, returns the padded size).
- Stream DMA (NES video) copies the frame buffer into RAM_G synchronously; `updateStreamDma` returns
  true once per frame.
- `getImgProps` returns zeros until 5b implements `CMD_GETPROPS`.
- `setRotation` (REG_ROTATE) is stored only.

## Semantics and assumptions (no Programmer Guide in the repo)

- Screen 800x480. Pixel (i, j) has its centre at (i + 0.5, j + 0.5).
- Graphics context (reset to defaults at the start of each list, saved/restored as a whole):
  colour (255,255,255,255), clear colour (0,0,0,0), line width 16, point size 16, scissor
  (0,0,2048,2048), blend (SRC_ALPHA, ONE_MINUS_SRC_ALPHA), bitmap handle 0, cell 0, transform
  A=E=256 others 0, vertex format 4, palette source 0, vertex translate 0, colour mask 1111,
  alpha func ALWAYS, stencil defaults, tag 0.
- Bitmap handle properties (`BITMAP_SOURCE`, `_LAYOUT(_H)`, `_SIZE(_H)`) are per-handle hardware
  registers, **not** part of the context, and persist across display lists (the core sets the font
  handles up once in `loadFont` and draws with them in later lists). `_H` registers are independent
  of the low-bit registers. `BITMAP_SIZE` width/height 0 means 2048 (FT81x 11-bit field).
- `LINE_WIDTH` and `POINT_SIZE` are radii in 1/16 px: a point is a disc of radius `pointSizePx`, a
  line is a capsule of radius `lineWidthPx` around the segment (total width 2x), a rectangle is the
  vertex box grown by `lineWidthPx` with corners rounded to that radius (Minkowski sum with a disc).
  `Prim::lineWidthPx` is `LINE_WIDTH / 16`.
- Antialiasing: coverage = clamp(r - d + 0.5, 0, 1) for distance d from a pixel centre to the
  primitive's core shape; coverage multiplies source alpha.
- Each element (point, line, strip segment, rectangle, edge-strip segment) blends separately.
- Blend: `out = src * f(srcFactor) + dst * f(dstFactor)` on RGBA in [0,1], factors ZERO, ONE,
  SRC_ALPHA, DST_ALPHA, ONE_MINUS_SRC_ALPHA, ONE_MINUS_DST_ALPHA.
- `CLEAR(c,..)` with no prims yet and a full-screen scissor sets `Frame::clearColor`; otherwise it
  becomes a `Rects` prim covering the scissor with blend (ONE, ZERO).
- Edge strips fill from the polyline to the screen edge (R: right, L: left, A: above, B: below),
  within each segment's span.
- `VERTEX2II` x, y are unsigned 9-bit pixels; handle and cell come from the vertex (bitmaps).
  `VERTEX_TRANSLATE` applies to both vertex forms.
- `CALL`/`JUMP` destinations are word indices into `ram_dl`; the call stack is 4 deep.
- A state change between the vertices of an open primitive starts a new `Prim`; an odd LINES/RECTS
  vertex or the last strip vertex carries over.
- `ram_dl` writes past 8 KB are dropped (logged once). Memory commands outside RAM_G and RAM_DL are
  ignored (logged once).
- `ramGGeneration(addr, len)` is the largest write stamp of the 4 KB pages in the range: it changes
  whenever any byte of the range may have changed.
