# vu1_cube

A gouraud shaded cube whose vertices are transformed on VU1 by a microprogram
adapted from ps2sdk `samples/draw/vu1/draw_3D.vsm`, sent to the GS with
`XGKICK`. The rotation is a pure function of the frame counter. Prints
`FRAME=n` every frame and runs forever; `test.toml` stops it after 240
vsyncs (one full turn about Y, two about X) and compares frames 60, 120 and
240. Display setup is shared with `gs_sprite` (see its README).

## Changes from the sample

* No texture: the program writes RGBAQ and XYZ2 per vertex (`NREG=2`) and
  colours are per vertex instead of one RGBA for the mesh.
* Scale and offset are separate vectors, so the projection maps NDC directly
  to GS coordinates (`x * 320 + 2048`, `y * -224 + 2048`, `z * 0xFFFFF`),
  all multiplied by 16 by `ftoi4`.
* The XGKICK buffer ends with a second GIF tag carrying a `FINISH` write, so
  the EE can `draw_wait_finish` on the VU1 output before printing the frame
  line.
* The loop counter is decremented well before the branch that reads it, and
  `Q` is only read seven cycles after `DIV`, matching the sample's spacing.
* The EE side does not use math3d: the local to clip matrix is built in C
  from a 240 entry sine table stored as exact hex float literals
  (`sin_table.c`, generated once), a camera 45 units back, near 30, far 60.
  With a half size of 10 every clip coordinate stays inside |w|, so the
  `clipw`/`fcand` path runs but never sets the ADC bit.

## VU memory layout

    qword 0..3       local to GS matrix, column major (unpacked to address 0)
    TOP+0            scale xyz, w = vertex count (int)
    TOP+1            offset
    TOP+2            GIF tag for the triangles (EOP=0, PRE=1, gouraud triangle PRIM, NREG=2: RGBAQ, XYZ2)
    TOP+3            GIF tag for FINISH (EOP=1, NLOOP=1, A+D)
    TOP+4            FINISH data
    TOP+5..          36 vertices, then 36 colours (R, G, B, A as 32 bit ints)
    TOP+77..         XGKICK buffer: 1 + 72 + 2 qwords

Double buffering uses `BASE=8`, `OFFSET=496`; each frame needs 152 qwords per
buffer.

## Per frame

1. `build_matrix(n)`: `Ry(2*pi*n/240) * Rx(2*pi*(2n+30)/240)`, translate by
   -45 in z, perspective with focal lengths 1.05 (x) and 1.5 (y), z mapped so
   the near plane is 1 and the far plane is 0 (the Z test is greater or
   equal, larger is nearer).
2. Clear colour and Z through the GIF channel, then `TEST` = Z greater or
   equal, `FINISH`, wait.
3. One VIF1 chain: `REF` + `UNPACK V4_32` of the matrix to address 0, of the
   five header qwords, the vertices and the colours to the current TOP
   buffer, then `FLUSH` + `MSCAL 0`, then an end tag. The EE waits for the
   VIF1 channel and then for the FINISH the microprogram kicks.
4. `FRAME=n`, `graph_wait_vsync`.

The microprogram is uploaded once with `MPG` through libpacket2's
`packet2_vif_add_micro_program`.
