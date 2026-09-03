import { readFileSync } from "node:fs";
import path from "node:path";
import { describe, expect, it } from "vitest";
import { compareCpu, compareTty, discoverFixtures, filterTty, parseFixtureToml, summarizeRun, ttyPayload } from "./support/fixtures";

const fixturesRoot = path.resolve("tests/fixtures");

describe("fixture test.toml", () => {
  it("lifts the PCSX2 extensions and validates the rest with the kit", () => {
    const config = parseFixtureToml(readFileSync(path.join(fixturesRoot, "hello_tty", "test.toml"), "utf8"));
    expect(config.kit.target).toBe("hello_tty.elf");
    expect(config.kit.frames).toBe(120);
    expect(config.kit.renderer).toEqual(["sw", "webgpu"]);
    expect(config.kit.cpu).toBe("interpreter");
    expect(config.kit.known_failure).toBe(false);
    expect(config.kit.bios).toBeUndefined();
    expect(config.biosRequired).toBe(true);
    expect(config.ttyFilter?.source).toBe("^[A-Z][A-Z0-9_]*(=.*)?$");
    expect(config.trace).toEqual({ tty: true, cpu: true, ramEvery: 60 });
    expect(config.kit.trace).toEqual({ ram_every: 60 });
    expect(config.kit.compare.tty).toEqual({ mode: "exact", path: "tty.txt", trigger: ["last_frame"], known_failure: false });
    expect(config.kit.compare.cpu?.mode).toBe("exact");
    expect(config.kit.compare.frames).toEqual({ mode: "md5", path: "frames", trigger: [60, 120], known_failure: false });
    expect(config.webgpuFrames).toEqual({ mode: "rmse", max_rmse: 1.0, min_close_pixels: 0.99, trigger: [60, 120] });
  });

  it("keeps a string bios and rejects a non string filter", () => {
    const config = parseFixtureToml('target = "x.elf"\nframes = 2\nbios = "ps2/scph39001.bin"\n');
    expect(config.kit.bios).toBe("ps2/scph39001.bin");
    expect(config.biosRequired).toBe(true);
    expect(config.trace).toEqual({ tty: true, cpu: false, ramEvery: 0 });
    expect(() => parseFixtureToml('target = "x.elf"\nframes = 2\nfilter = 3\n')).toThrow(/regular expression/);
    expect(() => parseFixtureToml('frames = 2\n')).toThrow(/target/);
  });

  it("discovers every fixture with its recorded oracle outputs", () => {
    const fixtures = discoverFixtures(fixturesRoot);
    expect(fixtures.map((fixture) => fixture.name)).toEqual(["gs_blend", "gs_sprite", "hello_tty", "vu1_cube"]);
    const hello = fixtures.find((fixture) => fixture.name === "hello_tty")!;
    expect(hello.targetUrl).toBe("tests/fixtures/hello_tty/hello_tty.elf");
    expect(hello.elfPath).toBe(path.join(fixturesRoot, "hello_tty", "hello_tty.elf"));
    expect(hello.expectedTtyPath).toBe(path.join(fixturesRoot, "hello_tty", "expected", "tty.txt"));
    expect(hello.expectedCpuPath).toBe(path.join(fixturesRoot, "hello_tty", "expected", "cpu.jsonl"));
    expect(hello.manifest?.renderer).toBe("Software");
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
