import { describe, expect, it } from "vitest";
import { validateReport } from "@appmana-public/web-emulator-harness/report";
import { DIGITAL1, DIGITAL2, INTERPRETER_SETTINGS, RENDERERS, STATUS, TtyDecoder, bytesToBase64, createRunReport, expandPadSchedule, isGsDumpTarget, mergePadState, packPadButtons, padPressureBytes, readAudioStats, rendererId, splitSettingKey, splitTraceRecords, statusName } from "../public/pcsx2-report.mjs";

describe("renderer selection", () => {
  it("selects the null renderer when rendering is off, WebGPU when it is on, and the software renderer by default", () => {
    expect(rendererId({ render: false })).toBe(11);
    expect(rendererId({ render: false, renderer: "sw" })).toBe(11);
    expect(rendererId({})).toBe(RENDERERS.sw);
    expect(rendererId({ render: true })).toBe(18);
    expect(rendererId({ render: true, renderer: "sw" })).toBe(13);
    expect(rendererId({ renderer: "SW" })).toBe(13);
    expect(rendererId({ renderer: "webgpu" })).toBe(RENDERERS.webgpu);
    expect(rendererId({ renderer: "null" })).toBe(11);
    expect(rendererId({ renderer: "auto" })).toBe(-1);
    expect(rendererId({ renderer: "21" })).toBe(21);
    expect(rendererId({ renderer: 14 })).toBe(14);
    expect(() => rendererId({ renderer: "d3d9" })).toThrow(/unknown renderer/);
  });

  it("recognises GS dump targets and encodes frame bytes", () => {
    expect(isGsDumpTarget("tests/fixtures/gs_sprite/expected/dumps/frame00450.gs.zst")).toBe(true);
    expect(isGsDumpTarget("a/b.gs.xz")).toBe(true);
    expect(isGsDumpTarget("a/b.gs")).toBe(true);
    expect(isGsDumpTarget("a/b.elf")).toBe(false);
    expect(bytesToBase64(new Uint8Array([0, 1, 2, 255]))).toBe(Buffer.from([0, 1, 2, 255]).toString("base64"));
    const big = new Uint8Array(100_000).map((_, index) => index & 0xff);
    expect(bytesToBase64(big)).toBe(Buffer.from(big).toString("base64"));
  });
});

describe("settings", () => {
  it("splits Section/Key at the last slash", () => {
    expect(splitSettingKey("EmuCore/GS/Renderer")).toEqual({ section: "EmuCore/GS", key: "Renderer" });
    expect(splitSettingKey("Folders/Bios")).toEqual({ section: "Folders", key: "Bios" });
    expect(() => splitSettingKey("Renderer")).toThrow(/Section\/Key/);
    expect(() => splitSettingKey("EmuCore/")).toThrow(/Section\/Key/);
  });

  it("forces the interpreter for every unit like the native oracle", () => {
    expect(Object.keys(INTERPRETER_SETTINGS)).toEqual([
      "EmuCore/CPU/Recompiler/EnableEE",
      "EmuCore/CPU/Recompiler/EnableIOP",
      "EmuCore/CPU/Recompiler/EnableVU0",
      "EmuCore/CPU/Recompiler/EnableVU1",
      "EmuCore/CPU/Recompiler/EnableEECache",
    ]);
    expect(Object.values(INTERPRETER_SETTINGS).every((value) => value === "false")).toBe(true);
  });
});

describe("status codes", () => {
  it("names the host status codes", () => {
    expect(STATUS.Running).toBe(2);
    expect(STATUS.BootFailed).toBe(5);
    expect(statusName(0)).toBe("Idle");
    expect(statusName(6)).toBe("CPUThreadFailed");
    expect(statusName(42)).toBe("status 42");
  });
});

describe("TtyDecoder", () => {
  it("joins chunks that split lines and multi byte sequences", () => {
    const decoder = new TtyDecoder();
    const bytes = new TextEncoder().encode("EE: HELLO=héllo\r\nIOP: A=1\nEE: partial");
    decoder.push(bytes.subarray(0, 12));
    decoder.push(bytes.subarray(12, 13));
    decoder.push(bytes.subarray(13));
    expect(decoder.lines).toEqual(["EE: HELLO=héllo", "IOP: A=1"]);
    expect(decoder.pending).toBe("EE: partial");
    expect(decoder.bytes).toBe(bytes.byteLength);
    expect(decoder.finish()).toEqual(["EE: HELLO=héllo", "IOP: A=1", "EE: partial"]);
    expect(decoder.pending).toBe("");
  });
});

describe("createRunReport", () => {
  it("produces a report the kit validates with the TTY at the top level", () => {
    const report = createRunReport({
      ok: false,
      detail: "boot: VM boot failed (BootFailed)",
      bootResult: 0,
      moduleCreateMs: 12.5,
      frames: [{ index: 0, elapsedMs: 100 }],
      workingSet: { heapBytes: 1, poolBusy: 2, poolIdle: 3, poolTotal: 5, frameCount: 1, ttyBytes: 4, traceBytes: 0 },
      shutdown: { stoppedCleanly: true, stopMs: 3 },
      tty: ["EE: HELLO=x"],
      emu: { cpuJsonl: "{\"frame\":0}\n" },
    });
    expect(report.schema).toBe(1);
    expect(report.emulator).toBe("pcsx2");
    expect(report.tty).toEqual(["EE: HELLO=x"]);
    expect(report.emu.cpuJsonl).toBe("{\"frame\":0}\n");
    expect(report.events).toEqual([]);
    expect(validateReport(report)).toEqual({ ok: true, errors: [] });
  });
});

describe("pad states for the host", () => {
  it("packs digital1 and digital2 into one button word", () => {
    expect(packPadButtons({ digital1: DIGITAL1.up | DIGITAL1.start, digital2: DIGITAL2.cross })).toBe(0x4018);
    expect(packPadButtons({ digital1: 0xff, digital2: 0xff })).toBe(0xffff);
  });

  it("merges partial states like setPad and resolves pressures", () => {
    const first = mergePadState(undefined, { digital2: DIGITAL2.cross, leftX: 0 });
    expect(first).toEqual({ digital1: 0, digital2: DIGITAL2.cross, leftX: 0, leftY: 127, rightX: 127, rightY: 127, pressure: { cross: 255 } });
    const second = mergePadState(first, { digital1: DIGITAL1.left, pressure: { cross: 0x40 } });
    expect(second).toMatchObject({ digital1: DIGITAL1.left, digital2: DIGITAL2.cross, leftX: 0, pressure: { left: 255, cross: 0x40 } });
    const third = mergePadState(second, { digital2: 0 });
    expect(third.pressure).toEqual({ left: 255 });
    // right, left, up, down, triangle, circle, cross, square, l1, r1, l2, r2
    expect([...padPressureBytes(second)]).toEqual([0, 255, 0, 0, 0, 0, 0x40, 0, 0, 0, 0, 0]);
    expect(mergePadState(undefined, { leftX: 999, rightY: -3 })).toMatchObject({ leftX: 255, rightY: 0 });
  });

  it("expands frame-indexed entries per port in frame order", () => {
    const schedule = expandPadSchedule([
      { frame: 10, digital2: DIGITAL2.cross },
      { frame: 5, port: 1, leftX: 0 },
      { frame: 10, digital1: DIGITAL1.up },
      { frame: 20, digital2: 0 },
    ]);
    expect(schedule.map((entry) => [entry.frame, entry.port, entry.digital1, entry.digital2, entry.leftX])).toEqual([
      [5, 1, 0, 0, 0],
      [10, 0, 0, DIGITAL2.cross, 127],
      [10, 0, DIGITAL1.up, DIGITAL2.cross, 127],
      [20, 0, DIGITAL1.up, 0, 127],
    ]);
    expect(() => expandPadSchedule([{ frame: -1 }])).toThrow(/frame/);
    expect(() => expandPadSchedule([{ frame: 1, port: 2 }])).toThrow(/port/);
  });
});

describe("audio records", () => {
  it("splits the host trace into cpu.jsonl and audio.jsonl", () => {
    const text = '{"frame":0,"ee":"a","iop":"b","vu0":"c","vu1":"d"}\n{"frame":0,"frames":0,"hash":"2d06800538d394c2"}\n{"frame":1,"ee":"a","iop":"b","vu0":"c","vu1":"d"}\n{"frame":1,"frames":768,"hash":"d758bee327ee22e6"}\n';
    expect(splitTraceRecords(text)).toEqual({
      cpuJsonl: '{"frame":0,"ee":"a","iop":"b","vu0":"c","vu1":"d"}\n{"frame":1,"ee":"a","iop":"b","vu0":"c","vu1":"d"}\n',
      audioJsonl: '{"frame":0,"frames":0,"hash":"2d06800538d394c2"}\n{"frame":1,"frames":768,"hash":"d758bee327ee22e6"}\n',
    });
  });

  it("reads the worklet counters in WebAudio::StatsIndex order", () => {
    const heap = new Uint32Array(new SharedArrayBuffer(64));
    heap.set([0xffffffff, 48000, 128, 7, 896, 300, 2, 1, 1536], 4);
    expect(readAudioStats(heap, 16)).toEqual({ state: -1, sampleRate: 48000, quantum: 128, callbacks: 7, pulledFrames: 896, nonzeroFrames: 300, underruns: 2, idleCallbacks: 1, writtenFrames: 1536 });
  });
});
