# pad_echo

Echoes the DualShock 2 state once per vsync and plays a tone, so pad input
and audio output can be checked end to end against the native oracle.

The ELF loads the BIOS's own `rom0:SIO2MAN` and `rom0:PADMAN`, opens port 0
with ps2sdk's libpad, locks the pad into DualShock (analog) mode and enables
pressure readings (`PADMODE=0x79`). It also loads `rom0:LIBSD` and the
ps2sdk `audsrv.irx` embedded in the ELF (`audsrv_irx.S`, `.incbin` from the
pinned toolchain image) and streams two integer triangle waves (about 440 Hz
left, 657 Hz right) at 48 kHz, so the SPU2 output is not silence.

Every vsync prints two lines:

    PAD=ffff LX=7f LY=7f RX=7f RY=7f
    PRESS=000000000000000000000000

`PAD` is the raw button word (active low, `ffff` idle; bit layout as in
libpad: select 0x0001 .. left 0x0080, l2 0x0100 .. square 0x8000), then the
left and right stick bytes. `PRESS` is the twelve pressure bytes in the pad
protocol order: right, left, up, down, triangle, circle, cross, square, l1,
r1, l2, r2.

## Input recording

`input.p2m2` is written by `web/scripts/pad-echo-input.mjs` with the kit's
`encodeP2m2` from a list of frame-indexed entries (presses, stick moves, a
one-frame tap, custom pressures, a second-port entry). The native oracle
replays it with `-input`; the browser lane decodes the same file into a pad
schedule (`tests/support/fixtures.ts`, `inputTraceFromP2m2`). PCSX2's replay
reads recording frame `i` at the vsync whose `g_FrameCount` is `i - 1`
(the frame counter is incremented before the frame's data is read) and pauses
the VM when the counter reaches the recording's total, so the recording is two
frames longer than the run and frame 0 is never applied.

## Recording the oracle

    node web/scripts/pad-echo-input.mjs
    yarn oracle:record -n 400 -b ~/.config/PCSX2/bios -o tests/fixtures/pad_echo/expected \
        tests/fixtures/pad_echo/pad_echo.elf -- -renderer sw -input tests/fixtures/pad_echo/input.p2m2 \
        -audio-hash -trace-ram-every 100

The tracerunner copies the recording into the trace directory; that copy is
removed after recording since `input.p2m2` next to `test.toml` is the source.
`expected/audio.jsonl` hashes, per vsync, every frame the SPU2 produced; the
browser's Web Audio stream records the same hash from the chunks it is handed.
