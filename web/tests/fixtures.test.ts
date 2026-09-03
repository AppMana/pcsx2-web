import { readFileSync } from "node:fs";
import path from "node:path";
import { describe, expect, it } from "vitest";
import { DIGITAL1, DIGITAL2, encodeP2m2 } from "@appmana-public/web-emulator-harness/p2m2";
import { compareAudio, compareCpu, compareTty, discoverFixtures, filterTty, inputTraceFromP2m2, parseFixtureToml, summarizeRun, ttyPayload } from "./support/fixtures";

const fixturesRoot = path.resolve("tests/fixtures");

describe("fixture test.toml", () => {
  it("reads the converged kit schema from the hello_tty fixture", () => {
    const config = parseFixtureToml(readFileSync(path.join(fixturesRoot, "hello_tty", "test.toml"), "utf8"));
    expect(config.kit.target).toBe("hello_tty.elf");
    expect(config.kit.frames).toBe(600);
    expect(config.kit.renderer).toEqual(["sw", "webgpu"]);
    expect(config.kit.cpu).toBe("interpreter");
    expect(config.kit.known_failure).toBe(false);
    expect(config.kit.bios).toEqual({ required: true });
    expect(config.biosRequired).toBe(true);
    expect(config.ttyFilter?.source).toBe("^[A-Z][A-Z0-9_]*(=.*)?$");
    expect(config.trace).toEqual({ tty: true, cpu: true, ramEvery: 100, audio: false });
    expect(config.kit.compare.tty?.mode).toBe("exact");
    expect(config.kit.compare.cpu?.mode).toBe("exact");
    expect(config.kit.compare.frames?.mode).toBe("md5");
    expect(config.kit.compare.frames?.trigger).toEqual([500, 600]);
    expect(config.webgpuFrames).toMatchObject({ mode: "rmse", max_rmse: 1.0, min_close_pixels: 0.99, trigger: [500, 600] });
  });

  it("keeps a string bios and rejects a bad line filter", () => {
    const config = parseFixtureToml('target = "x.elf"\nframes = 2\nbios = "ps2/scph39001.bin"\n');
    expect(config.kit.bios).toEqual({ required: true, path: "ps2/scph39001.bin" });
    expect(config.biosRequired).toBe(true);
    expect(config.trace).toEqual({ tty: true, cpu: true, ramEvery: 0, audio: false });
    expect(config.ttyFilter).toBeUndefined();
    expect(() => parseFixtureToml('target = "x.elf"\nframes = 2\n[compare.tty]\nmode = "exact"\nline_filter = "("\n')).toThrow(/line_filter/);
    expect(() => parseFixtureToml('frames = 2\n')).toThrow(/target/);
  });

  it("discovers every fixture with its recorded oracle outputs", () => {
    const fixtures = discoverFixtures(fixturesRoot);
    expect(fixtures.map((fixture) => fixture.name)).toEqual(["gs_blend", "gs_sprite", "hello_tty", "pad_echo", "vu1_cube"]);
    const hello = fixtures.find((fixture) => fixture.name === "hello_tty")!;
    expect(hello.targetUrl).toBe("tests/fixtures/hello_tty/hello_tty.elf");
    expect(hello.elfPath).toBe(path.join(fixturesRoot, "hello_tty", "hello_tty.elf"));
    expect(hello.expectedTtyPath).toBe(path.join(fixturesRoot, "hello_tty", "expected", "tty.txt"));
    expect(hello.expectedCpuPath).toBe(path.join(fixturesRoot, "hello_tty", "expected", "cpu.jsonl"));
    expect(hello.manifest?.renderer).toBe("Software");
    expect(hello.inputPath).toBeUndefined();
    expect(hello.expectedAudioPath).toBeUndefined();
  });

  it("binds pad_echo's input recording and audio oracle", () => {
    const pad = discoverFixtures(fixturesRoot).find((fixture) => fixture.name === "pad_echo")!;
    expect(pad.config.trace).toEqual({ tty: true, cpu: true, ramEvery: 100, audio: true });
    expect(pad.inputPath).toBe(path.join(fixturesRoot, "pad_echo", "input.p2m2"));
    expect(pad.expectedAudioPath).toBe(path.join(fixturesRoot, "pad_echo", "expected", "audio.jsonl"));
    expect(pad.manifest?.input).toMatch(/pad_echo\/input\.p2m2$/);
  });
});

describe("input recording to pad schedule", () => {
  const recording = encodeP2m2({
    entries: [
      { frame: 3, digital2: DIGITAL2.cross },
      { frame: 5, digital1: DIGITAL1.up, leftX: 10, pressure: { cross: 0x40 } },
      { frame: 5, port: 1, digital2: DIGITAL2.circle },
      { frame: 8, digital1: 0, digital2: 0, leftX: 127 },
    ],
    totalFrames: 12,
  });

  it("applies recording frame i at guest frame i - 1 and skips frame 0", () => {
    const entries = inputTraceFromP2m2(recording, 12);
    expect(entries.map((entry) => [entry.frame, entry.port])).toEqual([[0, 0], [0, 1], [2, 0], [4, 0], [4, 1], [7, 0]]);
    expect(entries[0]).toMatchObject({ frame: 0, port: 0, digital1: 0, digital2: 0, leftX: 127, leftY: 127, rightX: 127, rightY: 127, pressure: {} });
    expect(entries[2]).toMatchObject({ frame: 2, digital2: DIGITAL2.cross, pressure: { cross: 255 } });
    expect(entries[3]).toMatchObject({ frame: 4, port: 0, digital1: DIGITAL1.up, digital2: DIGITAL2.cross, leftX: 10, pressure: { up: 255, cross: 0x40 } });
    expect(entries[4]).toMatchObject({ frame: 4, port: 1, digital2: DIGITAL2.circle, pressure: { circle: 255 } });
    expect(entries[5]).toMatchObject({ frame: 7, digital1: 0, digital2: 0, leftX: 127, pressure: {} });
  });

  it("keeps only the run's frames", () => {
    expect(inputTraceFromP2m2(recording, 5).map((entry) => entry.frame)).toEqual([0, 0, 2, 4, 4]);
    expect(inputTraceFromP2m2(recording, 4).map((entry) => entry.frame)).toEqual([0, 0, 2]);
  });
});

describe("audio comparison", () => {
  it("compares the first frames of the oracle's audio.jsonl", () => {
    const expected = '{"frame":0,"frames":0,"hash":"2d06800538d394c2"}\n{"frame":1,"frames":768,"hash":"d758bee327ee22e6"}\n{"frame":2,"frames":768,"hash":"0000000000000000"}\n';
    expect(compareAudio(expected, '{"frame":0,"frames":0,"hash":"2d06800538d394c2"}\n{"frame":1,"frames":768,"hash":"d758bee327ee22e6"}\n', 2).ok).toBe(true);
    const verdict = compareAudio(expected, '{"frame":0,"frames":0,"hash":"2d06800538d394c2"}\n{"frame":1,"frames":768,"hash":"ffffffffffffffff"}\n', 2);
    expect(verdict.ok).toBe(false);
    expect(verdict.reason).toMatch(/^audio\.jsonl diverges at record 2/);
  });
});

describe("TTY filtering", () => {
  const filter = /^[A-Z][A-Z0-9_]*(=.*)?$/;

  it("strips the oracle's channel prefix before filtering", () => {
    expect(ttyPayload("EE: HELLO=hello_tty")).toBe("HELLO=hello_tty");
    expect(ttyPayload("IOP: Input ELF format filename = host:hello_tty.elf")).toBe("Input ELF format filename = host:hello_tty.elf");
    expect(ttyPayload("HELLO=x")).toBe("HELLO=x");
    expect(filterTty(["EE: # Initialize GS ...", "EE: HELLO=hello_tty", "IOP:  ROMGEN=2006-0905", "FPU_ADD_TIE=0x3f800000", "DONE"], filter))
      .toEqual(["HELLO=hello_tty", "FPU_ADD_TIE=0x3f800000", "DONE"]);
    expect(filterTty(["EE: a", "b"], undefined)).toEqual(["a", "b"]);
  });

  it("keeps the fixture lines of the recorded oracle", () => {
    const expected = readFileSync(path.join(fixturesRoot, "hello_tty", "expected", "tty.txt"), "utf8");
    const kept = filterTty(expected.split("\n"), filter);
    expect(kept[0]).toBe("HELLO=hello_tty");
    expect(kept.length).toBeGreaterThan(10);
    expect(kept.every((line) => filter.test(line))).toBe(true);
  });

  it("compares the filtered lines exactly and reports the first divergence", () => {
    const expected = "EE: # boot\nEE: HELLO=x\nEE: A=1\nEE: B=2\n";
    expect(compareTty(expected, ["HELLO=x", "A=1", "B=2"], filter)).toMatchObject({ ok: true, expectedLines: 3, actualLines: 3 });
    expect(compareTty(expected, ["EE: HELLO=x", "EE: A=1", "EE: B=2", "EE: # shutdown"], filter).ok).toBe(true);
    const short = compareTty(expected, ["HELLO=x"], filter);
    expect(short.ok).toBe(false);
    expect(short.firstDivergence).toEqual({ line: 2, expected: "A=1", actual: undefined });
    expect(short.reason).toContain("filtered line 2");
    const wrong = compareTty(expected, ["HELLO=x", "A=2", "B=2"], filter);
    expect(wrong.firstDivergence).toEqual({ line: 2, expected: "A=1", actual: "A=2" });
    const empty = compareTty("EE: # nothing matches\n", ["HELLO=x"], filter);
    expect(empty.ok).toBe(false);
    expect(empty.reason).toMatch(/matches no expected TTY line/);
  });
});

describe("CPU trace comparison", () => {
  it("compares the first `frames` oracle records and names the changed keys", () => {
    const expected = '{"frame":0,"ee":"a","eeram":"r"}\n{"frame":1,"ee":"b"}\n{"frame":2,"ee":"c"}\n';
    expect(compareCpu(expected, '{"frame":0,"ee":"a","eeram":"r"}\n{"frame":1,"ee":"b"}\n', 2)).toMatchObject({ ok: true, expectedRecords: 2, actualRecords: 2, expectedRecordsTotal: 3 });
    const diverged = compareCpu(expected, '{"frame":0,"ee":"a","eeram":"r"}\n{"frame":1,"ee":"x"}\n', 2);
    expect(diverged.ok).toBe(false);
    expect(diverged.firstDivergence?.line).toBe(2);
    expect(diverged.firstDivergence?.changedKeys).toEqual(["ee"]);
    expect(diverged.reason).toContain("record 2 (keys ee)");
    const missing = compareCpu(expected, "", 2);
    expect(missing.firstDivergence).toMatchObject({ line: 1, actual: undefined });
  });
});

describe("summarizeRun", () => {
  it("explains a failed boot with the TTY captured so far and the divergence", () => {
    const summary = summarizeRun("hello_tty", {
      ok: false,
      detail: "boot: VM boot failed (BootFailed)",
      bootResult: 0,
      tty: ["EE: # Initialize memory", "EE: # Initialize GS ..."],
      shutdown: { stoppedCleanly: true },
      emu: { statusName: "BootFailed", observedFrames: 0, requestedFrames: 120, trace: { supported: false }, logs: ["GSopen failed"] },
    }, { tty: compareTty("EE: HELLO=x\n", ["EE: # Initialize memory"], /^[A-Z][A-Z0-9_]*(=.*)?$/) });
    expect(summary).toContain("hello_tty: FAILED; boot: VM boot failed (BootFailed)");
    expect(summary).toContain("status BootFailed · bootResult 0 · frames 0/120 · shutdown clean");
    expect(summary).toContain("cpu trace not exported by this build");
    expect(summary).toContain("tty: 2 lines captured, last 2:");
    expect(summary).toContain("  | EE: # Initialize GS ...");
    expect(summary).toContain('tty comparison: TTY diverges at filtered line 1: expected "HELLO=x", actual undefined (0/1 lines)');
    expect(summary).toContain("  > GSopen failed");
  });
});
