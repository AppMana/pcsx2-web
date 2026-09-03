import { describe, expect, it } from "vitest";
import { validateReport } from "@appmana-public/web-emulator-harness/report";
import { INTERPRETER_SETTINGS, RENDERERS, STATUS, TtyDecoder, bytesToBase64, createRunReport, isGsDumpTarget, rendererId, splitSettingKey, statusName } from "../public/pcsx2-report.mjs";

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
