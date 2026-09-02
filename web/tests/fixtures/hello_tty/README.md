# hello_tty

Prints a fixed sequence of `KEY=VALUE` lines to the EE console, then one
`FRAME=n` line per vsync for 120 vsyncs, then `DONE`, then waits for vsyncs
forever. The display is initialised and cleared to a dark blue once so that
frame captures are well defined.

## Which console path, and why

PCSX2 has three ways a program's text can reach `ConsoleLogging.eeConsole`
(`pcsx2/DebugTools/Debug.h`, `eeConLog` macro at line 261):

1. Byte stores to the EE SIO transmit FIFO, `SIO_TXFIFO = 0x1000F180`
   (`pcsx2/Hw.h` line 313). `_hwWrite8` in `pcsx2/HwWrite.cpp` lines 287
   to 318 collects bytes, treats `\r` as a newline and drops the `\n` that
   follows it, and flushes a line to `eeConLog` on newline or at 1024 bytes.
   32 bit stores to the same address are split into four byte stores (lines
   145 to 155).
2. The `sysPrintOut` syscall (number 117, `pcsx2/R5900OpcodeTables.h` line
   22), HLE'd in `SYSCALL()` in `pcsx2/R5900OpcodeImpl.cpp` lines 1139 to
   1189: the format string is rendered with the host `snprintf`, only the
   first seven register arguments are read, and `%s` arguments are remapped
   from guest addresses. The exception is still raised afterwards (lines
   1204 to 1205), so the BIOS kernel runs the call as well.
3. `Deci2Call` (syscall 124) with `a0 == 0x10`, lines 1128 to 1136.

This fixture uses path 1 through libkernel's `sio_init`, `sio_puts` and
`sio_putc` (`ps2sdk/ee/include/sio.h`). The disassembly of `sio_putc` in
`libkernel.a` polls `SIO_ISR` (`0x1000F130`) until `(value & 0xF000) !=
0x8000`, which PCSX2's read handler satisfies immediately (`pcsx2/HwRead.cpp`
lines 127 to 143 return 0), then does `sb` to `0x1000F180`. `sio_putc('\n')`
emits `\r\n`, which PCSX2 collapses to one newline. Path 1 was chosen because
it is plain memory mapped I/O: it does not depend on the syscall HLE's host
`snprintf`, on the seven argument limit, or on the BIOS kernel's own
implementation of the same call.

The ordinary ps2sdk `printf` was not used: libcglue's stdio goes to
`fioWrite` over SIF RPC to the IOP, so it appears on PCSX2's IOP console, not
the EE console.

## Line format

* `HELLO=hello_tty`
* `FPU_*=0x%08x`, `VU0_*_X=0x%08x` and so on: raw IEEE single bit patterns
* `FPU_CVTW_*=0x%08x`: raw 32 bit integers from `cvt.w.s`
* `FRAME=n` for n in 0..119, one per `graph_wait_vsync`
* `DONE`

All lines match the `filter` regex in `test.toml`, `^[A-Z][A-Z0-9_]*(=.*)?$`.

## Floating point cases

Every operation is a single inline assembly instruction (`add.s`, `sub.s`,
`mul.s`, `div.s`, `sqrt.s`, `rsqrt.s`, `adda.s` + `madd.s`/`msub.s`,
`cvt.w.s`, `cvt.s.w`, `max.s`, `min.s`, `abs.s`, `neg.s`) so the compiler
cannot fold or reorder anything. Operands were chosen so that round to
nearest even and chop to zero produce different bit patterns wherever the
exact result is not representable. Reference values computed with exact
rational arithmetic (the EE FPU chops, so the chop column is what real
hardware and a correct emulator print):

| Line | nearest | chop |
|---|---|---|
| `FPU_ADD_1P5ULP` (1 + 3*2^-25) | 0x3f800001 | 0x3f800000 |
| `FPU_ADD_TIE` (1 + 2^-24) | 0x3f800000 | 0x3f800000 |
| `FPU_SUB_1P25ULP` (1 - 5*2^-26) | 0x3f7fffff | 0x3f7ffffe |
| `FPU_MUL_1P5ULP_SQ` (0x3fc00001 squared) | 0x40100002 | 0x40100001 |
| `FPU_MUL_1P23_1P4` | 0x3fdc6a7f | 0x3fdc6a7e |
| `FPU_DIV_1_3` | 0x3eaaaaab | 0x3eaaaaaa |
| `FPU_DIV_2_3` | 0x3f2aaaab | 0x3f2aaaaa |
| `FPU_DIV_10_7` | 0x3fb6db6e | 0x3fb6db6d |
| `FPU_SQRT_5` | 0x400f1bbd | 0x400f1bbc |
| `FPU_SQRT_123456P789` | 0x43afae9e | 0x43afae9d |
| `FPU_MADD_1_1P23_1P4` (product rounded, then sum rounded) | 0x402e3540 | 0x402e353f |
| `FPU_MSUB_1_1P23_1P4` | 0xbf38d4fe | 0xbf38d4fc |
| `FPU_CVTS_INT_MAX` (2^31 - 1 to float) | 0x4f000000 | 0x4effffff |
| `FPU_CVTS_2P24_3` (2^24 + 3 to float) | 0x4b800002 | 0x4b800001 |

Cases where both modes agree (`FPU_ADD_TIE`, `FPU_SQRT_2`, `FPU_RSQRT_1_2`)
are kept as controls. The madd/msub lines also separate a fused
implementation from the two step rounding the EE performs.

Denormal and range cases:

* `FPU_DAZ_ADD`, `FPU_DAZ_MUL`, `FPU_DAZ_MAX`: 2^-127 (bits 0x00400000) as an
  input. With denormals flushed on input the results are 0; an IEEE unit
  would give 0x00800000, 0x00800000 and 0x00400000.
* `FPU_FTZ_MUL`, `FPU_FTZ_DIV`, `FPU_FTZ_SUB`: results that are denormal
  under IEEE (0x00400000, 0x00200000, 0x00000001) and 0 with flush to zero.
* `FPU_OVF_MUL`, `FPU_OVF_ADD`, `FPU_OVF_NEG_MUL`, `FPU_DIV_1_0`,
  `FPU_DIV_M1_0`, `FPU_DIV_0_0`, `FPU_RSQRT_1_0`: the EE has no infinity or
  NaN and clamps to 0x7f7fffff or 0xff7fffff.
* `FPU_SQRT_M4`: the EE takes the square root of the magnitude.
* `FPU_CVTW_3P5`, `FPU_CVTW_M3P5`, `FPU_CVTW_2P7`: 3, -3 (0xfffffffd) and 2
  under truncation; 4, -4 and 3 under round to nearest.
  `FPU_CVTW_1E10` and `FPU_CVTW_M1E10` saturate to 0x7fffffff and
  0x80000000.
* `FPU_MAX_MZERO_ZERO`, `FPU_NEG_ZERO`, `FPU_ABS_M1P5`: sign handling.

## VU0 macro mode

`vu0_run` loads six vectors with `lqc2` and runs `vadd`, `vsub`, `vmul`,
`vmax`, `vmini`, `vmula`/`vmadd`, `vdiv` + `vwaitq` + `vmulq`, `vsqrt`,
`vrsqrt`, `vftoi0`, `vitof0`, `vopmula`/`vopmsub` and `vmulax`/`vmaddy`.
Each lane of an input vector is one test case, so `VU0_ADD_X` is the FPU
`FPU_ADD_1P5ULP` case on VU0, `VU0_ADD_Y` is the 1.5 ulp product operands
added, `VU0_ADD_Z` is 1.23 + 1.4 and `VU0_ADD_W` is denormal + denormal.
The `VU0_OVF_*` and `VU0_DIV_MAX_*` lines rerun the same program with
overflow and underflow operands. `Q` results are printed from the w lane of
`vf0 * Q`, which is `Q` itself because `vf0.w` is 1.

## Frame capture

`test.toml` compares frames 60 and 120. The screen is a solid colour, so the
comparison mostly checks that display setup (SetGsCrt through
`graph_set_mode`, DISPFB/DISPLAY through `graph_set_framebuffer_filtered`)
behaves identically.
