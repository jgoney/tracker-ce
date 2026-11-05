# FT812 usage inventory (living document)

What the core asks of the FT812, and how the Mac model (`Ft812Model` + `CoproDecoder` + `DlDecoder`)
handles it. Counts are call sites in `core/` outside `core/libs/FT812/` (grep of `API_*`, Task 5a).
Task 5b added text, numbers, fonts, bitmaps (texel decoding), gradients, matrices, snapshots and
image loading; the "Status" columns say what the model does now.

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
only `CMD_SWAP` decodes `ram_dl` (up to `DISPLAY`) into a `Frame`. Widget-level coprocessor commands
(`CMD_TEXT`, `CMD_NUMBER`, `CMD_GRADIENT`, `CMD_SETFONT2`, `CMD_SETBITMAP`, `CMD_SETMATRIX`,
`CMD_LOADIMAGE` without `OPT_NODL`) expand into display-list words too, never into prims, so the
copies the core caches with `CMD_MEMCPY` and replays with `CMD_APPEND` keep them.

## Display-list instructions

| Instruction | Uses | Status |
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

| Command | Uses | Where | Status |
|---|---|---|---|
| `CMD_TEXT` | 58 | 21 files | glyph words, see "Text" below |
| `CMD_GRADIENT` | 26 | bars, lists, tracker shadows | model-private DL instruction, see "Gradients" |
| `CMD_DLSTART` | 13 | | `dlOffset = 0` |
| `CMD_SWAP` | 8 | | decode `ram_dl`, publish `Frame` |
| `CMD_NUMBER` | 8 | `songPlayer.cpp` | formatted in the `CMD_SETBASE` base, then drawn as `CMD_TEXT` |
| `CMD_APPEND` | 8 | | copy RAM_G -> `ram_dl[dlOffset]`, advance |
| `CMD_SETBITMAP` | 6 (3 live) | `label.cpp:330`, `battery.cpp:120`, `img.cpp:111`, `stream.cpp:120` | `BITMAP_SOURCE`, `BITMAP_LAYOUT(_H)`, `BITMAP_SIZE(NEAREST, BORDER, BORDER)(_H)` for the current handle; stride from the format |
| `CMD_MEMCPY` | 6 | widget cache | implemented (RAM_G and RAM_DL address ranges) |
| `CMD_TRANSLATE` / `CMD_ROTATE` / `CMD_LOADIDENTITY` / `CMD_SETMATRIX` | 4 / 2 / 2 / 2 | rotated text `label.cpp:209-213`, `songPlayer.cpp:643-647` | coprocessor matrix; `CMD_SETMATRIX` writes `BITMAP_TRANSFORM_A`-`F`, see "Matrices" (`CMD_SCALE` too, unused) |
| `CMD_LOADIMAGE` + `CMD_GETPROPS` | 1 + 1 | image viewer `display.cpp:419,456-460` | JPEG/PNG parsed segment by segment, decoded with ImageIO to RGB565 (L8 with `OPT_MONO`) in RAM_G; `getImgProps` returns (end address, width, height); without `OPT_NODL` also the `CMD_SETBITMAP` words |
| `CMD_SNAPSHOT2` | 2 | screenshots `snapshots.cpp:85,100` (fmt 0x20 ARGB8) | `Ft812Model::snapshotHook(Rect, addr, fmt)`; unset or false: the area is filled opaque black |
| `CMD_MEMZERO` | 1 | `snapshots.cpp:117` | implemented |
| `CMD_SETFONT2` | 2 | font upload `display.cpp:286,1369` | registers the font, writes the handle's bitmap words, see "Text" |
| `CMD_SETBASE` | 2 | `songPlayer.cpp:240,244` (hex numbers) | stored (persists across lists) |
| `CMD_SETFONT` | 0 | | registers the font (first character 0), no words |
| `CMD_MEMSET` / `CMD_MEMWRITE` | 0 | | implemented (brief) |

No `CMD_INFLATE` (fonts and images are RLE-decoded on the CPU, `display.cpp:240,265,309`).
`mtTest.cpp` (factory test) also uses `CMD_TEXT` with ROM font 28 and `CMD_GRADIENT`.
Every other command (`CMD_BUTTON`, `CMD_CLOCK`, `CMD_ROMFONT`, ...: none is sent by the core outside
`mtTest.cpp`) is logged once per opcode and skipped by the byte length the core's encoder
(`FT812.cpp`) gives it.

`CMD_TEXT` options used: `OPT_CENTERY` 56, `OPT_CENTERX` 35, `OPT_RIGHTX` 33, `OPT_CENTER` 18.

## Bitmap formats

- From the core's tables: L4 (26 uses: fonts, icons), L2 (16: synth logos), L8 (2: boot logo).
- RGB565: image viewer (`img.cpp:111`, after `CMD_LOADIMAGE`) and NES video stream (`stream.cpp`,
  format 7 when `byte_res == 2`, else 4 = RGB332).
- PALETTED4444: only in a commented-out line (`img.cpp:105`); would need `PALETTE_SOURCE`.
- `Bitmaps::decode` (RGBA8, 0xRRGGBBAA) supports every FT81x bitmap format with whole-byte or
  sub-byte pixels: L1, L2, L4, L8 (white, alpha = L; sub-byte pixels leftmost in the most
  significant bits), RGB332, ARGB2, ARGB1555, ARGB4, RGB565 (16-bit little endian), PALETTED565,
  PALETTED4444 (8-bit index, 16-bit palette entries), PALETTED8 (32-bit entries B, G, R, A).
  n-bit channels expand to 8 bits by `round(v * 255 / (2^n - 1))`. TEXT8X8, TEXTVGA, BARGRAPH
  and FT80x PALETTED decode transparent (unused).
- `CpuRaster` samples texels: cell `n` starts at `(source + n * stride * height) mod 2^22`;
  screen offset `(x, y)` from the vertex (pixel centre) maps to texel
  `(A x + B y + C, D x + E y + F)`; NEAREST takes the texel containing it, BILINEAR blends the
  four nearest texel centres; BORDER is transparent black outside the layout, REPEAT wraps; only
  pixels inside `BITMAP_SIZE` are drawn. Texel colour is multiplied by `COLOR_RGB`/`COLOR_A`,
  then blended like any primitive.

## Driver-level calls

- `writeDataRAMG` (bitmaps at boot), `writeFontRAMG` (fonts: bytes 144..147 of the 148-byte metric
  block are replaced with `dest + 148`, as `API_LIB_WriteFontRAMG` does), `readDataRAMG`
  (screenshots), `readBackDataRAMG` (unused; byte-reversed read starting at `src - 1`, mirrored).
- `writeDataToCMD` (image viewer) appends to the command stream and executes it at once, like the IMX
  driver which bumps REG_CMD_WRITE itself.
- `sendString` mirrors `API_SendString` (length 0 = strlen, NUL, pad to 4, returns the padded size).
- Stream DMA (NES video) copies the frame buffer into RAM_G synchronously; `updateStreamDma` returns
  true once per frame.
- `getImgProps` returns the last `CMD_LOADIMAGE`'s (end address, width, height); zeros before.
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
- `LINE_WIDTH` and `POINT_SIZE` are radii in 1/16 px: a point is a disc of radius `pointRadiusPx`,
  a line is a capsule of radius `lineRadiusPx` around the segment (total width 2x), a rectangle is
  the vertex box grown by `lineRadiusPx` with corners rounded to that radius (Minkowski sum with a
  disc). `Prim::lineRadiusPx` is `LINE_WIDTH / 16`, `Prim::pointRadiusPx` is `POINT_SIZE / 16`
  (5b renamed them from `lineWidthPx`/`pointSizePx`, which read like full widths).
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

## Text (Task 5b)

- `CMD_SETFONT2(font, ptr, first)` registers the 148-byte metric block at `ptr` (widths for
  characters 0..127 at bytes 0..127, then format, stride, width, height and the glyph pointer as
  32-bit words at 128..144) and writes `BITMAP_HANDLE(font)`, `BITMAP_SOURCE(glyphs - first *
  stride * height)` (22-bit wrap), `BITMAP_LAYOUT(fmt, stride, height)`, `BITMAP_LAYOUT_H`,
  `BITMAP_SIZE(NEAREST, BORDER, BORDER, width, height)`, `BITMAP_SIZE_H`. With that source, cell
  = character code, which is what the core's own `VERTEX2II(x, y, font, ch)` (vertical labels,
  tracker) relies on. `CMD_SETFONT` registers with first character 0 and writes nothing.
- `CMD_TEXT(x, y, font, options, s)`: string width = sum of the metric widths; `OPT_CENTERX`
  subtracts width / 2, else `OPT_RIGHTX` subtracts width; `OPT_CENTERY` subtracts height / 2
  (integer division). Emits `BEGIN(BITMAPS)`, one `VERTEX2II(x, y, font, ch)` per drawn glyph,
  advancing x by the glyph's width, then `END()`. If any glyph is outside VERTEX2II's 0..511 range
  the whole string is wrapped in `SAVE_CONTEXT`, `BITMAP_HANDLE(font)`, `VERTEX_FORMAT(0)` ...
  `RESTORE_CONTEXT`, and those glyphs use `CELL(ch)` + `VERTEX2F(x, y)` (one more context level;
  the core nests at most 2). Characters below the font's first character or with width 0 advance
  but emit nothing; characters >= 128 are not drawn (no extended-font paging; logged once).
- Fonts not registered by `CMD_SETFONT(2)` (the ROM fonts 16..31, only used by `mtTest.cpp`) draw
  nothing (logged once per handle).
- `CMD_NUMBER`: digits in the `CMD_SETBASE` base (2..36, default 10), **upper-case** letters
  (`255` in base 16 is "FF"); `OPT_SIGNED` prints negatives with '-'; options bits 0..7 give a
  minimum digit count padded with '0'. Then drawn as `CMD_TEXT` with the same options.
- Verified on the boot screens: start screen ("Opening last project...", "v1.0.0 CE") and the
  pattern editor (title bar, track headers, row numbers, bottom bar labels and values) render
  readable text; 956 prims, no display-list overflow.

## Gradients (Task 5b)

`CMD_GRADIENT(x0, y0, rgb0, x1, y1, rgb1)` is written to `ram_dl` as 5 words: the model-private
header `(0x3E << 24) | 4` (`DlDecoder::kGradientHeader`; opcode 0x3E is unused by FT81x and BT81x)
followed by the 4 argument words verbatim. `DlDecoder` turns it into a `Prim::Gradient` with
`verts = {p0, p1}`, `rgba = rgb0 | 0xFF`, `rgba2 = rgb1 | 0xFF`, and the current scissor and blend
mode. It fills the scissor: colour = lerp(rgb0, rgb1, t) with t the projection of the pixel centre
onto p0 -> p1, clamped to [0, 1] (p0 == p1: rgb0). Colours are opaque; `COLOR_RGB`/`COLOR_A` do
not apply. Real EVE draws gradients with its own words (and dithers); the core always scissors
them (`label.cpp`, `bar.cpp`, `tracker.cpp` shadows), except `mtTest.cpp` (full screen).

## Matrices (Task 5b)

The coprocessor keeps a 2x3 matrix M mapping bitmap to screen coordinates, reset by
`CMD_LOADIDENTITY` (and at construction; it persists across lists). `CMD_TRANSLATE(tx, ty)`,
`CMD_SCALE(sx, sy)` (16.16) and `CMD_ROTATE(a)` (1/65536 turn, clockwise on the y-down screen:
`[cos -sin; sin cos]`) post-multiply M. `CMD_SETMATRIX` writes M's inverse (screen -> texel) as
`BITMAP_TRANSFORM_A..F`, rounded to 8 fractional bits; a singular matrix writes nothing (logged).
So `CMD_SCALE(2, 2)` gives A = E = 128 (drawn twice as large), and `label.cpp`'s
translate(c)/rotate(270 deg)/translate(-c) gives A = 0, B = -1, C = 2c, D = 1, E = 0, F = 0:
glyph tops face left, text reads bottom to top.

## Snapshots and images (Task 5b)

- `CMD_SNAPSHOT2(fmt, ptr, x, y, w, h)` calls `snapshotHook(Rect{x, y, w', h}, ptr, fmt)` with
  `w' = w / 2` for ARGB8 (0x20: the core passes 1600 for the 800 px screen and reads 3200-byte
  rows), else `w`. Without a hook (or if it returns false) the area is filled with opaque black
  in that format (ARGB8 bytes 00 00 00 FF). Task 9 provides the real hook.
- `CMD_LOADIMAGE(ptr, options)`: the following stream bytes are the file (the core sends it with
  `writeDataToCMD`, each chunk padded to 4). The model walks JPEG marker segments (lengths, then
  entropy data until a non-RST marker, until EOI) or PNG chunks (until IEND), so a FF D9 inside
  EXIF/APPn data does not end the image; then the stream resumes at the next 4-byte boundary.
  Data that is neither JPEG nor PNG is logged and the bytes run as commands again. Decoding uses
  ImageIO (also on iOS) without EXIF orientation, sRGB, to RGB565 (5/6/5 truncation) for both
  JPEG and PNG (EVE picks other formats for some PNGs; the core only shows JPEG/PNG photos as
  RGB565, `img.cpp:111`), or L8 with `OPT_MONO`. PNG alpha is premultiplied onto black.
