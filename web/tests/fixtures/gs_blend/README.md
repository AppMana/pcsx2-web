# gs_blend

Cycles through GS alpha blending configurations, a procedurally generated
64x64 PSMCT32 texture and the Z test, changing mode every 30 frames. Prints
`MODE=m` when the mode changes (including `MODE=0` before the first frame)
and `FRAME=n` every frame. Runs forever; `test.toml` stops it after 240
vsyncs, which covers all eight modes once, and compares frames 60, 120 and
240. Display setup is shared with `gs_sprite` (see its README).

## Texture

`generate_texture` fills a 64x64 RGBA buffer: an 8 pixel checkerboard of
(0xff, 0x40, 0x20) and (0x20, 0x60, 0xff), alpha rising from 0 to 126 left to
right (`alpha = x * 2`), and a white diagonal stripe with alpha 0x80 wherever
`(x + y) mod 16 == 0`.

`upload_texture` sends one normal DMA packet: a PACKED tag (`NLOOP=4`,
`EOP=0`, A+D) with `BITBLTBUF` (destination base = VRAM address / 64,
destination width 1 block of 64 pixels, PSMCT32), `TRXPOS` (0, 0),
`TRXREG` (64 x 64), `TRXDIR` 0 (host to local); then an IMAGE tag
(`FLG=2`, `NLOOP=1024`, `EOP=1`) followed by the 1024 qwords of pixels in
row major order; then `TEXFLUSH` and `FINISH`. The VRAM address comes from
`graph_vram_allocate` after the frame and Z buffers, block aligned.

Each frame sets `TEX0` (address / 64, width 1, PSMCT32, 2^6 x 2^6, RGBA,
modulate), `TEX1` (LOD from K, nearest filtering) and `CLAMP` (repeat in
both directions) with a single PACKED tag of three A+D writes.

## Modes

Blend colour is `(A - B) * C >> 7 + D`, with 0 = source colour, 1 =
destination colour, 2 = zero for A, B and D, and 0 = source alpha, 1 =
destination alpha, 2 = FIX for C. `ABE` is the blending enable bit in PRIM.

| MODE | ABE | A | B | C | D | FIX | Z test | textured |
|---|---|---|---|---|---|---|---|---|
| 0 | 0 | 0 | 1 | 0 | 1 | 0x80 | always pass | yes |
| 1 | 1 | 0 | 1 | 0 | 1 | 0x80 | always pass | yes |
| 2 | 1 | 0 | 2 | 0 | 1 | 0x80 | always pass | yes |
| 3 | 1 | 2 | 1 | 0 | 1 | 0x80 | always pass | yes |
| 4 | 1 | 0 | 1 | 2 | 1 | 0x40 | always pass | no |
| 5 | 1 | 0 | 1 | 2 | 1 | 0xc0 | greater or equal | no |
| 6 | 1 | 0 | 1 | 1 | 1 | 0x80 | greater or equal | yes |
| 7 | 1 | 1 | 0 | 0 | 2 | 0x80 | greater or equal | yes |

Mode 1 is the usual source alpha blend, 2 is additive, 3 darkens by the
source alpha, 4 and 5 use the FIX constant (0xc0 is above 1.0 and relies on
COLCLAMP), 6 blends by the alpha already in the frame buffer, and 7 computes
`(Cd - Cs) * As`. The alpha test is disabled in every mode (libdraw's default
`draw_enable_tests` would discard alpha 0 pixels, so `TEST` is written
explicitly each frame).

## Scene, per frame n

1. Clear to (0x20, 0x20, 0x20) at Z=0 with Z test always pass, then `TEST`
   for the mode, then `ALPHA` for the mode, then the texture registers.
2. Near textured quad (Z=0x00300000, alpha 0x40) at (224,160) to (480,416),
   drawn first.
3. Far textured quad (Z=0x00100000, alpha 0x60) at (96,64) to (352,320),
   overlapping the near quad. Its U coordinates scroll by 16 texels per
   frame (`(n * 16) & 1023` in 12.4), which repeats through `CLAMP`. With
   the Z test on, the far quad loses the overlap; with it off, it wins.
4. A wide tinted strip that repeats the texture four times horizontally and
   twice vertically at Z=0x00200000.
5. Two gouraud triangles at Z=0x00200000 with vertex alphas 0x80, 0x40 and
   0x00 (never textured).
6. A small white flat quad in front of everything (Z=0x00400000, never
   textured).

Textured quads are sprites with `FST=1` and `UV` coordinates in 12.4 fixed
point: PACKED tag `NLOOP=6`, A+D: PRIM, RGBAQ, UV, XYZ2, UV, XYZ2. When a
mode has `TME=0` the UV writes are harmless and the quad is flat coloured.
Triangles are PACKED `NLOOP=7`: PRIM (IIP=1), then RGBAQ, XYZ2 three times.
