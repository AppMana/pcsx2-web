# Homebrew PS2 test fixtures

These are small ps2sdk programs that the pcsx2-web oracle harness runs both
natively (pcsx2-tracerunner) and in the wasm build. Each fixture lives in its
own directory with committed sources, a Makefile, the built ELF, a
`test.toml` describing the run, a `README.md`, and an `expected/` directory
that the native oracle fills in with recorded outputs.

| Fixture | Exercises | Frames |
|---|---|---|
| `hello_tty` | EE SIO console output, EE FPU rounding, denormal and overflow behaviour, VU0 macro mode | 120 |
| `gs_sprite` | flat sprites, gouraud triangles, alpha blending on a 640x448 interlaced frame buffer | 240 |
| `gs_blend` | GS alpha blending equations, FIX values, a 64x64 RGBA texture upload, the Z test | 240 |
| `vu1_cube` | VU1 microprogram upload, VIF unpack and MSCAL, XGKICK, a rotating gouraud cube | 240 |
| `pad_echo` | DualShock 2 input through SIO2MAN/PADMAN replayed from `input.p2m2`, SPU2 output through audsrv | 400 |
| `hello_tty_iso` | `hello_tty` booted from a disc image (ISO and CHD) through the BIOS and the CDVD reader | 120 |

`hello_tty_iso` holds no sources of its own: `web/scripts/build-disc-fixtures.sh`
wraps the committed `hello_tty.elf` in an ISO 9660 image with a `SYSTEM.CNF`
and converts it to CHD with chdman; see its README for the exact commands.

## Building

The ELFs are committed so test lanes never need the toolchain. To rebuild
after a source change run

    web/scripts/build-fixtures.sh [fixture ...]

The script pins `ps2dev/ps2dev` by image digest, adds Alpine's `make` in a
derived local image, builds each fixture with the ps2sdk sample Makefile
conventions, strips the result, and checks with `readelf` that the output is
an ELF32 little endian MIPS executable with a LOAD segment. Two consecutive
runs produce byte identical ELFs; the script prints the sha256 of each.

## Common code

`common/` holds what every fixture shares:

* `fixture_tty.c`: console output through libkernel's `sio_putc`, which
  stores bytes to the EE SIO transmit FIFO at `0x1000F180`. PCSX2 line
  buffers exactly that address into its EE console. Every line has the form
  `KEY=VALUE`; a harness keeps only the fixture's own lines with the
  `line_filter` regex under `[compare.tty]` in `test.toml`.
* `fixture_gs.c`: the shared display, NTSC interlaced FIELD mode with a
  640x448 PSMCT32 frame buffer and a 32 bit Z buffer, plus helpers that emit
  single A+D register writes and a full screen clear.
* `fixture.mk`: make rules, include paths and the strip step.

## test.toml

    target = "name.elf"        # ELF to boot, relative to the fixture directory
    frames = 800               # vsyncs to run from power-on, BIOS boot included
    bios = true                # a real BIOS image is required (never committed)
    renderer = ["sw", "webgpu"]
    cpu = "interpreter"
    input = ""                 # .p2m2 replay, empty for none
    known_failure = false      # self baseline instead of a hard failure

    [trace]                    # what the tracerunner records
    tty = true
    cpu = true                 # per vsync register hashes
    ram_every = 100            # RAM hash every N frames, 0 disables

    [compare.tty]  mode = "exact"
    [compare.cpu]  mode = "exact"
    [compare.frames]        mode = "md5",  trigger = [frames...]   # sw renderer
    [compare.frames.webgpu] mode = "rmse", max_rmse, min_close_pixels, trigger

`compare.frames` is the md5 comparison used for the software renderer;
`compare.frames.webgpu` overrides it for the WebGPU renderer with an RMSE and
close pixel fraction threshold.

## Determinism rules

No fixture reads a timer or anything random. Animation is a pure function of
the frame counter, and every floating point value that reaches the console is
printed as its raw bit pattern. `pad_echo` reads the pad, whose state comes
from the fixture's own input recording on both sides of the comparison.
