// Fixture discovery and the toml-driven expectations for the e2e specs. The
// fixture test.toml files use the kit's converged schema (boolean `bios`,
// `[trace]`, `line_filter` under `[compare.tty]`, per-renderer overrides under
// `[compare.frames]`), so the kit loader is used directly.
import { existsSync, readFileSync, readdirSync, statSync } from "node:fs";
import path from "node:path";
import { frameCompareFor, parseTestToml, type CompareSpec, type TestConfig } from "@appmana-public/web-emulator-harness/test-toml";
import { exactText, jsonlHashDiff } from "@appmana-public/web-emulator-harness/compare";

export type WebGpuFrameCompare = CompareSpec;

export type Pcsx2TestConfig = {
  kit: TestConfig;
  biosRequired: boolean;
  ttyFilter: RegExp | undefined;
  trace: { tty: boolean; cpu: boolean; ramEvery: number };
  webgpuFrames: WebGpuFrameCompare | undefined;
};

/** A disc image next to a fixture's target that boots to the same oracle. */
export type FixtureImage = {
  name: string;
  path: string;
};

export type Fixture = {
  name: string;
  dir: string;
  config: Pcsx2TestConfig;
  /** "elf" boots the target as the ELF override; "disc" boots it through the BIOS from storage. */
  kind: "elf" | "disc";
  elfPath: string;
  /** URL of the ELF relative to the served pages (the preview server maps /tests/fixtures/ to the repository tree). */
  targetUrl: string;
  /** For disc fixtures: the target first, then every other ISO/BIN/CHD in the directory (recorded to the same expected/). */
  images: FixtureImage[];
  expectedDir: string;
  expectedTtyPath: string | undefined;
  expectedCpuPath: string | undefined;
  manifest: Record<string, unknown> | undefined;
};

export function parseFixtureToml(text: string): Pcsx2TestConfig {
  const kit = parseTestToml(text);
  return {
    kit,
    biosRequired: kit.bios.required,
    ttyFilter: kit.compare.tty?.lineFilter,
    trace: { tty: kit.trace.tty, cpu: kit.trace.cpu, ramEvery: kit.trace.ram_every },
    webgpuFrames: frameCompareFor(kit, "webgpu"),
  };
}

const DISC_IMAGE = /\.(iso|bin|chd)$/i;

export function loadFixture(dir: string, fixturesRoot = path.resolve("tests/fixtures")): Fixture {
  const config = parseFixtureToml(readFileSync(path.join(dir, "test.toml"), "utf8"));
  const name = path.basename(dir);
  const expectedDir = path.join(dir, "expected");
  const expectedTtyPath = path.join(expectedDir, config.kit.compare.tty?.path ?? "tty.txt");
  const expectedCpuPath = path.join(expectedDir, config.kit.compare.cpu?.path ?? "cpu.jsonl");
  const manifestPath = path.join(expectedDir, "manifest.json");
  const kind = /\.elf$/i.test(config.kit.target) ? "elf" : "disc";
  const images: FixtureImage[] = kind === "disc"
    ? [config.kit.target, ...readdirSync(dir).filter((entry) => entry !== config.kit.target && DISC_IMAGE.test(entry)).sort()]
      .map((entry) => ({ name: entry, path: path.join(dir, entry) }))
    : [];
  return {
    name,
    dir,
    config,
    kind,
    elfPath: path.join(dir, config.kit.target),
    targetUrl: path.posix.join("tests/fixtures", path.relative(fixturesRoot, dir).split(path.sep).join("/"), config.kit.target),
    images,
    expectedDir,
    expectedTtyPath: existsSync(expectedTtyPath) ? expectedTtyPath : undefined,
    expectedCpuPath: existsSync(expectedCpuPath) ? expectedCpuPath : undefined,
    manifest: existsSync(manifestPath) ? (JSON.parse(readFileSync(manifestPath, "utf8")) as Record<string, unknown>) : undefined,
  };
}

/** Every directory under the fixtures root with a test.toml, sorted by name. */
export function discoverFixtures(fixturesRoot = path.resolve("tests/fixtures")): Fixture[] {
  return readdirSync(fixturesRoot)
    .sort()
    .map((entry) => path.join(fixturesRoot, entry))
    .filter((dir) => statSync(dir).isDirectory() && existsSync(path.join(dir, "test.toml")))
    .map((dir) => loadFixture(dir, fixturesRoot));
}

// The native oracle prefixes every console line with its channel ("EE: ",
// "IOP: "); the fixture filter regexes describe the guest's own KEY=VALUE
// lines. The payload after the channel prefix is what gets filtered and
// compared, on both sides.
export const CHANNEL_PREFIX = /^(EE|IOP): /;

export function ttyPayload(line: string): string {
  return line.replace(CHANNEL_PREFIX, "");
}

export function filterTty(lines: string[], filter: RegExp | undefined): string[] {
  return lines.map(ttyPayload).filter((line) => !filter || filter.test(line));
}

export function splitLines(text: string): string[] {
  const lines = text.replaceAll("\r\n", "\n").split("\n");
  if (lines.at(-1) === "") lines.pop();
  return lines;
}

export type TtyVerdict = {
  ok: boolean;
  reason: string | undefined;
  expectedLines: number;
  actualLines: number;
  firstDivergence: { line: number; expected: string | undefined; actual: string | undefined } | undefined;
  expectedFiltered: string[];
  actualFiltered: string[];
};

export function compareTty(expectedText: string, actualLines: string[], filter: RegExp | undefined): TtyVerdict {
  const expectedFiltered = filterTty(splitLines(expectedText), filter);
  const actualFiltered = filterTty(actualLines, filter);
  if (filter && expectedFiltered.length === 0) {
    return { ok: false, reason: `filter ${filter} matches no expected TTY line; the comparison would be empty`, expectedLines: 0, actualLines: actualFiltered.length, firstDivergence: undefined, expectedFiltered, actualFiltered };
  }
  const result = exactText(expectedFiltered.join("\n"), actualFiltered.join("\n"));
  const divergence = result.firstDivergence;
  return {
    ok: result.ok,
    reason: result.ok ? undefined : divergence
      ? `TTY diverges at filtered line ${divergence.line}: expected ${JSON.stringify(divergence.expected)}, actual ${JSON.stringify(divergence.actual)} (${result.actualLines}/${result.expectedLines} lines)`
      : "TTY differs",
    expectedLines: result.expectedLines,
    actualLines: result.actualLines,
    firstDivergence: divergence,
    expectedFiltered,
    actualFiltered,
  };
}

export type CpuVerdict = ReturnType<typeof jsonlHashDiff> & { reason: string | undefined; expectedRecordsTotal: number };

// The oracle may have recorded more frames than test.toml asks for; the run
// covers `frames` vsyncs, so the reference is the first `frames` records.
export function compareCpu(expectedJsonl: string, actualJsonl: string, frames: number): CpuVerdict {
  const expectedAll = splitLines(expectedJsonl);
  const expected = expectedAll.slice(0, frames);
  const result = jsonlHashDiff(`${expected.join("\n")}\n`, actualJsonl);
  const divergence = result.firstDivergence;
  return {
    ...result,
    expectedRecordsTotal: expectedAll.length,
    reason: result.ok ? undefined : divergence
      ? `cpu.jsonl diverges at record ${divergence.line}${divergence.changedKeys ? ` (keys ${divergence.changedKeys.join(", ")})` : ""}: expected ${JSON.stringify(divergence.expected)}, actual ${JSON.stringify(divergence.actual)} (${result.actualRecords}/${result.expectedRecords} records)`
      : "cpu.jsonl differs",
  };
}

export type RunReportLike = {
  ok?: boolean;
  detail?: string;
  bootResult?: number;
  frames?: unknown[];
  tty?: string[];
  shutdown?: { stoppedCleanly?: boolean; detail?: string };
  emu?: { statusName?: string; observedFrames?: number; requestedFrames?: number; trace?: { supported?: boolean }; cpuJsonl?: string; logs?: string[] };
};

/** A readable failure summary: the run's own verdict, the TTY captured so far, and the first divergence. */
export function summarizeRun(name: string, report: RunReportLike, verdicts: { tty?: TtyVerdict; cpu?: CpuVerdict; ttyTail?: number } = {}): string {
  const ttyTail = verdicts.ttyTail ?? 30;
  const tty = report.tty ?? [];
  const lines = [
    `${name}: ${report.ok ? "ran" : "FAILED"}; ${report.detail ?? "no detail"}`,
    `status ${report.emu?.statusName ?? "unknown"} · bootResult ${report.bootResult ?? "n/a"} · frames ${report.emu?.observedFrames ?? report.frames?.length ?? 0}/${report.emu?.requestedFrames ?? "?"} · shutdown ${report.shutdown?.stoppedCleanly ? "clean" : `not clean (${report.shutdown?.detail ?? "no report"})`}`,
    `cpu trace ${report.emu?.trace?.supported ? `supported, ${splitLines(report.emu?.cpuJsonl ?? "").length} records` : "not exported by this build"}`,
    `tty: ${tty.length} lines captured${tty.length ? `, last ${Math.min(ttyTail, tty.length)}:` : ""}`,
    ...tty.slice(-ttyTail).map((line) => `  | ${line}`),
  ];
  if (verdicts.tty) lines.push(`tty comparison: ${verdicts.tty.ok ? "match" : verdicts.tty.reason} (${verdicts.tty.actualLines} actual / ${verdicts.tty.expectedLines} expected filtered lines)`);
  if (verdicts.cpu) lines.push(`cpu comparison: ${verdicts.cpu.ok ? "match" : verdicts.cpu.reason}`);
  const logs = report.emu?.logs ?? [];
  if (!report.ok && logs.length) lines.push(`module log tail:`, ...logs.slice(-10).map((line) => `  > ${line}`));
  return lines.join("\n");
}
