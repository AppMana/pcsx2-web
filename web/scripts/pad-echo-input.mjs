#!/usr/bin/env node
// Writes the input recording the pad_echo fixture is recorded and replayed
// with: web/tests/fixtures/pad_echo/input.p2m2, a PCSX2 .p2m2 (file version
// 1) encoded by the kit from the frame-indexed entries below. The native
// oracle replays the file through PCSX2's input recording; the browser lane
// decodes the same file into a pad schedule (tests/support/fixtures.ts,
// inputTraceFromP2m2). Entries merge into the port's state like setPad().
//
// Usage: node scripts/pad-echo-input.mjs [output.p2m2]
import { writeFileSync } from "node:fs";
import path from "node:path";
import { fileURLToPath } from "node:url";
import { DIGITAL1, DIGITAL2, encodeP2m2 } from "@appmana-public/web-emulator-harness/p2m2";

// The run is 400 vsyncs (test.toml frames); the recording is two frames
// longer because the replay pauses the VM when its frame counter reaches
// the recording's total.
export const PAD_ECHO_FRAMES = 400;
export const PAD_ECHO_RECORDING_FRAMES = PAD_ECHO_FRAMES + 2;

export const PAD_ECHO_ENTRIES = [
  { frame: 150, digital2: DIGITAL2.cross },
  { frame: 170, digital1: DIGITAL1.up },
  { frame: 190, digital1: 0, digital2: 0, leftX: 0x20, leftY: 0xe0, rightX: 0xff, rightY: 0x00 },
  { frame: 210, digital1: DIGITAL1.start | DIGITAL1.select, digital2: DIGITAL2.square | DIGITAL2.l2, leftX: 127, leftY: 127, rightX: 127, rightY: 127, pressure: { square: 0x40, l2: 0x80 } },
  { frame: 230, digital1: DIGITAL1.left | DIGITAL1.r3, digital2: DIGITAL2.triangle | DIGITAL2.r1, pressure: { left: 0x10 } },
  { frame: 250, digital1: 0, digital2: 0 },
  { frame: 300, digital2: DIGITAL2.circle },
  { frame: 301, digital2: 0 },
  { frame: 320, port: 1, digital2: DIGITAL2.cross },
  { frame: 340, digital1: DIGITAL1.down | DIGITAL1.right, digital2: DIGITAL2.l1 | DIGITAL2.r2, leftX: 0xff, leftY: 0x40 },
  { frame: 370, digital1: 0, digital2: 0, leftX: 127, leftY: 127 },
];

export function encodePadEchoRecording() {
  return encodeP2m2({ entries: PAD_ECHO_ENTRIES, totalFrames: PAD_ECHO_RECORDING_FRAMES, gameName: "pad_echo", author: "pcsx2-web" });
}

if (process.argv[1] && path.resolve(process.argv[1]) === fileURLToPath(import.meta.url)) {
  const output = process.argv[2] ?? path.resolve(path.dirname(fileURLToPath(import.meta.url)), "../tests/fixtures/pad_echo/input.p2m2");
  const bytes = encodePadEchoRecording();
  writeFileSync(output, bytes);
  process.stdout.write(`${output}: ${bytes.length} bytes, ${PAD_ECHO_RECORDING_FRAMES} frames, ${PAD_ECHO_ENTRIES.length} entries\n`);
}
