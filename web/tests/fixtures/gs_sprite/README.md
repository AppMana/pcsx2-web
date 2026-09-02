# gs_sprite

Draws a fixed scene of flat shaded sprites and gouraud triangles with alpha
blending on the shared 640x448 PSMCT32 interlaced (NTSC, FIELD mode) frame
buffer, then prints `FRAME=n` to the EE console (see `../hello_tty/README.md`
for the console path) and waits for vsync. Runs forever; `test.toml` stops it
after 240 vsyncs and compares frames 60, 120 and 240.

## Display mode

`fixture_gs_init` in `../common/fixture_gs.c` programs the CRTC the same way
ps2sdk's `graph_initialize` does (`graph_set_mode`, `graph_set_screen`,
`graph_set_bgcolor`, `graph_set_framebuffer_filtered`, `graph_enable_output`)
but with the region fixed to NTSC and the flicker filter off, so the mode does
not depend on the BIOS region. FIELD mode with a 640x448 buffer means each
field reads every other line of the same buffer; the whole buffer is redrawn
every vsync before the FRAME line is printed, so the captured frame is
complete.

## Scene, per frame n

All coordinates are integers; the only thing that changes with n is the
position of two sprites and one triangle vertex.

1. Full screen clear to (0x18, 0x18, 0x20) at Z=0 with the Z test set to
   always pass (writes both colour and Z).
2. `ALPHA` = A: source, B: destination, C: source alpha, D: destination,
   FIX 0x80, that is `(Cs - Cd) * As >> 7 + Cd`.
3. Five opaque flat sprites (red, green, blue, yellow, cyan) in a row.
4. A translucent white band (alpha 0x40, ABE=1) and a translucent grey band
   (alpha 0x60) across the row.
5. An opaque white sprite at x = (n * 4) mod 560 and a translucent magenta
   sprite (alpha 0x20) moving the other way at half speed.
6. An opaque gouraud triangle with red, green and blue corners, a flat
   triangle (IIP=0, so only the last vertex colour is used), a translucent
   gouraud triangle whose vertex alphas go 0x80, 0x40, 0x00, and a
   translucent gouraud triangle whose apex x = 320 + (n * 3 mod 200) - 100.

## GIF packets

Everything is one normal mode DMA transfer to the GIF per frame, built in
`build_frame`. Every primitive is a PACKED tag with `NREG=1`, the register
list `A+D`, and `NLOOP` equal to the number of register writes that follow:

* sprite: `NLOOP=4`: PRIM (sprite, IIP=0, ABE from the call), RGBAQ, XYZ2,
  XYZ2. The second XYZ2 kicks the sprite.
* triangle: `NLOOP=7`: PRIM (triangle, IIP and ABE from the call), then
  RGBAQ and XYZ2 three times. The third XYZ2 kicks the triangle.
* clear (`fixture_gs_clear`): `NLOOP=5`: TEST, PRIM, RGBAQ, XYZ2, XYZ2.
* single register (`fixture_gs_ad`): `NLOOP=1`.

Coordinates are 12.4 fixed point relative to the primitive origin
`XYOFFSET = (2048 - 320, 2048 - 224)`, so screen pixel (x, y) is written as
`((1728 + x) << 4, (1824 + y) << 4)`. The packet ends with the libdraw
`FINISH` tag and the EE polls CSR before printing the frame line.
