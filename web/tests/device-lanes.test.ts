// The iPad lanes without an iPad: the kit's device lanes accept an injectable
// session factory, so both wrappers run end to end against a scripted page
// (prerequisites, uploads, the driver's result, per-frame pixel reads, the
// cleanup) and the judges run on reports assembled from the fixtures' own
// oracle outputs.
import { cpSync, existsSync, mkdirSync, mkdtempSync, readFileSync, rmSync, writeFileSync } from "node:fs";
import { tmpdir } from "node:os";
import path from "node:path";
import { afterAll, beforeAll, describe, expect, it } from "vitest";
import { PNG } from "pngjs";
import { decodePng } from "@appmana-public/web-emulator-harness/compare";
import {
  CLEANUP_EXPRESSION,
  INJECTED_DRIVER,
  buildInjectedModules,
  checkDeviceQuiet,
  collectModuleGraph,
  collectStaticImports,
  judgeDumpFrames,
  judgeRun,
  measure,
  normalizeReport,
  resolveDeviceTarget,
  runInjectedDevice,
  runOptionsFor,
  runOriginDevice,
  watchPages,
} from "../scripts/device-lane-support.mjs";
import { splitLines } from "./support/fixtures";

const publicDir = path.resolve("public");
const fixturesRoot = path.resolve("tests/fixtures");
const device = { deviceName: "Benjamin’s iPad", deviceOSVersion: "26.5.2", url: "127.0.0.1:9222" };
const rpcs3Page = { title: "RPCS3", url: "https://rpcs3.appmana.com/runtime.html?device=1", webSocketDebuggerUrl: "ws://127.0.0.1:9222/devtools/page/1" };
const prerequisites = { secureContext: true, crossOriginIsolated: true, sharedArrayBuffer: true, webGpu: true, wasm: true, userAgent: "Safari", hardwareConcurrency: 8, url: rpcs3Page.url };
const capabilities = { gpuInWorker: true, gpuOnMain: true, offscreenCanvas: true, sharedArrayBuffer: true, crossOriginIsolated: true, hardwareConcurrency: 8, adapter: { vendor: "apple", architecture: "apple", device: "apple", description: "", isFallbackAdapter: false }, features: ["depth32float-stencil8", "shader-f16"] };
const appleAdapter = "WebGPU (WebGPU)\n0x0\nApple GPU";

let scratch: string;
let distDir: string;
beforeAll(() => {
  scratch = mkdtempSync(path.join(tmpdir(), "pcsx2-device-lanes-"));
  // A dist with the real page modules and kit files, and a stand-in core.
  distDir = path.join(scratch, "dist");
  mkdirSync(path.join(distDir, "core"), { recursive: true });
  for (const name of ["runtime-worker.mjs", "runtime-acceptance.mjs", "pcsx2-report.mjs"]) cpSync(path.join(publicDir, name), path.join(distDir, name));
  cpSync(path.join(publicDir, "kit"), path.join(distDir, "kit"), { recursive: true });
  writeFileSync(path.join(distDir, "core", "pcsx2-web.mjs"), 'var x=1;new Worker(new URL("pcsx2-web.mjs",import.meta.url),{type:"module",name:"em-pthread-"+PThread.nextWorkerID});export default createPCSX2;');
  writeFileSync(path.join(distDir, "core", "pcsx2-web.wasm"), Buffer.from([0, 0x61, 0x73, 0x6d, 1, 0, 0, 0]));
});
afterAll(() => rmSync(scratch, { recursive: true, force: true }));

type Evaluation = { expression: string; awaitPromise: boolean };

function fakeSession(answer: (expression: string, awaitPromise: boolean) => unknown, page = rpcs3Page) {
  const evaluations: Evaluation[] = [];
  let closed = 0;
  const connection = {
    evaluate: async (expression: string, awaitPromise = false) => {
      evaluations.push({ expression, awaitPromise });
      return answer(expression, awaitPromise);
    },
    command: async () => ({}),
    snapshotPng: async () => Buffer.from(PNG.sync.write(new PNG({ width: 1, height: 1 }))),
    close: () => { closed += 1; },
  };
  return { session: { device, page, connection }, evaluations, closedCount: () => closed };
}

const quietOptions = { quietMs: 1, sleep: async () => {}, processes: () => [] as string[], discover: async () => ({ device, pages: [rpcs3Page] }), watchPollMs: 10_000 };

/** A report the runtime would produce for the hello_tty fixture, from its oracle outputs. */
function helloTtyReport(overrides: Record<string, unknown> = {}) {
  const target = resolveDeviceTarget("hello_tty", fixturesRoot);
  if (target.kind !== "elf") throw new Error("elf target expected");
  const tty = splitLines(readFileSync(target.fixture.expectedTtyPath!, "utf8"));
  const cpuJsonl = `${splitLines(readFileSync(target.fixture.expectedCpuPath!, "utf8")).slice(0, target.frames).join("\n")}\n`;
  const frames = Array.from({ length: target.frames }, (_, index) => ({ index, elapsedMs: 1200 + index * 70 }));
  const workingSet = { heapBytes: 805306368, poolBusy: 2, poolIdle: 4, poolTotal: 6, frameCount: target.frames, ttyBytes: 4096, traceBytes: 80000 };
  return {
    schema: 1,
    emulator: "pcsx2",
    ok: true,
    detail: `ran ${target.frames} frames`,
    bootResult: 0,
    moduleCreateMs: 800,
    frames,
    events: [{ elapsedMs: 800, type: "module-created", moduleCreateMs: 800 }, { elapsedMs: 1100, type: "vm-running", bootMs: 1100 }],
    workingSet,
    shutdown: { stoppedCleanly: true, stopMs: 5, detail: "Running -> Idle", workingSet: { ...workingSet, poolBusy: 0 } },
    tty,
    emu: { target: target.targetUrl, isDump: false, observedFrames: target.frames, requestedFrames: target.frames, trace: { supported: true, cpu: true, ramEvery: 100, tty: true }, cpuJsonl, logs: [] },
    ...overrides,
  };
}

/** The native WebGPU renders of a dump as the pixels a device run would read back. */
function nativeFrames(target: ReturnType<typeof resolveDeviceTarget>) {
  if (target.kind !== "dump") throw new Error("dump target expected");
  return [...target.nativeFrames.entries()].sort((a, b) => a[0] - b[0]).map(([dumpFrame, png]) => ({ dumpFrame, rgba: decodePng(new Uint8Array(readFileSync(png))) }));
}

function dumpReport(target: ReturnType<typeof resolveDeviceTarget>, gsHost: "worker" | "main" = "worker") {
  const native = nativeFrames(target);
  const frames: Array<Record<string, unknown>> = [];
  const pixels = new Map<number, Uint8Array>();
  let index = 0;
  for (const dumpLoop of [1, 0]) {
    for (const { dumpFrame, rgba } of native) {
      frames.push({ index, elapsedMs: 1000 + index * 25, gpu: { width: rgba.width, height: rgba.height, frameHash: `h${dumpLoop}${dumpFrame}`, changedPixels: 1000, dumpFrame, dumpLoop, oracleFrame: 0, hasRgba: true }, hostTimings: { renderMs: 12.5 } });
      pixels.set(index, rgba.data);
      index += 1;
    }
  }
  const workingSet = { heapBytes: 805306368, poolBusy: 3, poolIdle: 3, poolTotal: 6, frameCount: index, ttyBytes: 0, traceBytes: 0 };
  const report = {
    schema: 1,
    emulator: "pcsx2",
    ok: true,
    detail: `ran ${index} frames`,
    bootResult: 0,
    moduleCreateMs: 900,
    frames,
    gpu: { adapter: appleAdapter, captured: index, readback: "async", gsHost },
    events: [{ elapsedMs: 900, type: "module-created", moduleCreateMs: 900 }, { elapsedMs: 1400, type: "vm-running", bootMs: 1400 }],
    workingSet,
    shutdown: { stoppedCleanly: true, stopMs: 1, detail: "Idle -> Idle", workingSet },
    tty: [],
    emu: { target: target.targetUrl, isDump: true, observedFrames: index, requestedFrames: 100_000, trace: { supported: true, cpu: false, ramEvery: 0, tty: true }, logs: [] },
  };
  return { report, pixels };
}

const frameIndexOf = (expression: string) => Number(/frame\.index === (\d+)/.exec(expression)?.[1]);

describe("device targets and run options", () => {
  it("resolves ELF fixtures and GS dumps from the fixture tree", () => {
    const elf = resolveDeviceTarget("hello_tty", fixturesRoot);
    expect(elf).toMatchObject({ kind: "elf", name: "hello_tty", targetUrl: "tests/fixtures/hello_tty/hello_tty.elf", frames: 600 });
    const dump = resolveDeviceTarget("gs_blend/frame00700.gs.zst", fixturesRoot);
    expect(dump).toMatchObject({ kind: "dump", name: "gs_blend-frame00700", dumpName: "frame00700", targetUrl: "tests/fixtures/gs_blend/expected/dumps/frame00700.gs.zst" });
    if (dump.kind !== "dump") throw new Error("dump expected");
    expect([...dump.nativeFrames.keys()]).toEqual([1, 2, 3, 4]);
    expect(dump.compare).toMatchObject({ mode: "rmse", max_rmse: 1, min_close_pixels: 0.99 });
    expect(resolveDeviceTarget("gs_blend/frame00700", fixturesRoot).localPath).toBe(dump.localPath);
    expect(() => resolveDeviceTarget("nope", fixturesRoot)).toThrow(/no fixture named "nope"/);
    expect(() => resolveDeviceTarget("gs_blend/frame99999", fixturesRoot)).toThrow(/no dump named "frame99999"/);
    expect(() => resolveDeviceTarget("hello_tty_iso", fixturesRoot)).toThrow(/disc image/);
  });

  it("sends the null renderer with the CPU trace for ELFs, WebGPU captures for dumps and rendered ELFs", () => {
    const elf = resolveDeviceTarget("hello_tty", fixturesRoot);
    expect(runOptionsFor(elf, { bios: "pcsx2/bios/x.bin" })).toEqual({ frames: 600, render: false, cpu: "interpreter", bios: "pcsx2/bios/x.bin", trace: { cpu: true, ramEvery: 100, tty: true }, timeoutMs: 600_000, pthreadPoolSize: 6 });
    const rendered = runOptionsFor(elf, { render: true, gsHost: "main", pthreadPoolSize: 4, timeoutMs: 1000 });
    expect(rendered).toMatchObject({ frames: 602, render: true, renderer: "webgpu", gsHost: "main", readback: "async", captureRgba: true, captureEvery: 0, captureFrames: [499, 500, 501, 599, 600, 601], trace: { cpu: false, ramEvery: 0, tty: true }, pthreadPoolSize: 4, timeoutMs: 1000 });
    const dump = resolveDeviceTarget("gs_blend/frame00700", fixturesRoot);
    expect(runOptionsFor(dump)).toEqual({ frames: 100_000, render: true, renderer: "webgpu", gsHost: undefined, readback: "async", captureRgba: true, captureEvery: 1, loops: 2, timeoutMs: 600_000, pthreadPoolSize: 6 });
  });
});

describe("injected module manifest", () => {
  it("collects static imports only", () => {
    expect(collectStaticImports('import { a } from "./a.js";\nexport * from "../b.js";\nimport "./c.js";\nconst x = await import(coreUrl);\n// from "./comment.js"\nimport {\n  d,\n} from "./d.js";')).toEqual(["./a.js", "../b.js", "./c.js", "./d.js"]);
  });

  it("walks the page modules' import graph in dependency order with placeholder rewrites", () => {
    const graph = collectModuleGraph(distDir, [{ name: "worker", relative: "runtime-worker.mjs" }, { name: "acceptance", relative: "runtime-acceptance.mjs" }]);
    const names = graph.map((module) => module.name);
    expect(names.at(-2)).toBe("worker");
    expect(names.at(-1)).toBe("acceptance");
    expect(names).toContain("pcsx2-report.mjs");
    expect(names).toContain("kit/disc-images/browser/opfs.js");
    expect(names).toContain("kit/disc-images/core/index.js");
    for (const module of graph) {
      for (const rewrite of module.rewrites) expect(names.indexOf(rewrite.toModule)).toBeLessThan(names.indexOf(module.name));
      expect(collectStaticImports(module.text).filter((specifier) => specifier.startsWith("."))).toEqual([]);
    }
    const worker = graph.find((module) => module.name === "worker")!;
    expect(worker.rewrites.map((rewrite) => rewrite.toModule)).toEqual(["pcsx2-report.mjs", "kit/disc-images/browser/opfs.js"]);
    expect(worker.text).toContain('from "__DEVICE_MODULE_kit_disc_images_browser_opfs_js__"');
  });

  it("builds the manifest: rewritten glue, page modules, kit helpers, wasm, target and BIOS", () => {
    const dump = resolveDeviceTarget("gs_blend/frame00700", fixturesRoot);
    const modules = buildInjectedModules({ distDir, target: dump });
    const names = modules.map((module) => module.name);
    expect(names.slice(-3)).toEqual(["core", "wasm", "target"]);
    const core = Buffer.from(modules.find((module) => module.name === "core")!.bytes!).toString("utf8");
    expect(core).toContain('new Worker(import.meta.url, {type:"module"');
    expect(core).not.toContain('new URL("pcsx2-web.mjs"');
    const acceptance = Buffer.from(modules.find((module) => module.name === "acceptance")!.bytes!).toString("utf8");
    expect(acceptance).toContain('new Worker("__DEVICE_MODULE_worker__"');
    expect(acceptance).toContain('"#pcsx2-device-status"');
    expect(acceptance).toContain('"pcsx2-device-canvas-host"');
    expect(modules.find((module) => module.name === "acceptance")!.rewrites).toEqual(expect.arrayContaining([{ from: "__DEVICE_MODULE_worker__", toModule: "worker" }, { from: "__DEVICE_MODULE_pcsx2_report_mjs__", toModule: "pcsx2-report.mjs" }]));
    expect(modules.find((module) => module.name === "wasm")).toMatchObject({ mime: "application/wasm" });
    expect(modules.find((module) => module.name === "target")!.bytes!.byteLength).toBe(readFileSync(dump.localPath).byteLength);
    expect(names).not.toContain("bios");

    const elf = resolveDeviceTarget("hello_tty", fixturesRoot);
    expect(() => buildInjectedModules({ distDir, target: elf })).toThrow(/PCSX2_BIOS/);
    const bios = path.join(scratch, "scph.bin");
    writeFileSync(bios, Buffer.alloc(16, 7));
    expect(buildInjectedModules({ distDir, target: elf, biosPath: bios }).at(-1)).toMatchObject({ name: "bios", mime: "application/octet-stream" });

    const broken = path.join(scratch, "dist-broken");
    cpSync(distDir, broken, { recursive: true });
    writeFileSync(path.join(broken, "core", "pcsx2-web.mjs"), "export default 1;");
    expect(() => buildInjectedModules({ distDir: broken, target: dump })).toThrow(/pthread worker constructor/);
  });
});

describe("judges and measurements", () => {
  it("passes an ELF run whose TTY and cpu.jsonl match the oracle, and names the first divergence otherwise", () => {
    const target = resolveDeviceTarget("hello_tty", fixturesRoot);
    const good = judgeRun(target, { report: helloTtyReport(), prerequisites, capabilities, frames: [] });
    expect(good.passed, JSON.stringify(good.checks.filter((check) => !check.ok))).toBe(true);
    expect(good.tty).toMatchObject({ ok: true });
    expect(good.cpu).toMatchObject({ ok: true, expectedRecords: 600, actualRecords: 600 });
    expect(good.checks.map((check) => check.name)).not.toContain(expect.stringMatching(/adapter/));

    const report = helloTtyReport();
    const first = report.tty.findIndex((line: string) => /^EE: [A-Z][A-Z0-9_]*=/.test(line));
    report.tty[first] = "EE: BROKEN=1";
    const bad = judgeRun(target, { report, prerequisites, capabilities, frames: [] });
    expect(bad.passed).toBe(false);
    expect(bad.tty?.reason).toMatch(/TTY diverges at filtered line 1/);
    expect(bad.checks.find((check) => check.name.startsWith("tty"))).toMatchObject({ ok: false });

    const noTrace = judgeRun(target, { report: helloTtyReport({ emu: { ...helloTtyReport().emu, cpuJsonl: undefined } }), prerequisites, capabilities, frames: [] });
    expect(noTrace.checks.find((check) => check.name.startsWith("cpu.jsonl"))).toMatchObject({ ok: false, detail: "the run exported no cpu.jsonl" });

    const aborted = judgeRun(target, { report: helloTtyReport(), prerequisites, capabilities, frames: [], aborted: "page list changed" });
    expect(aborted.passed).toBe(false);
    expect(aborted.checks[0]).toMatchObject({ name: "page list unchanged during the run", ok: false, detail: "page list changed" });

    expect(judgeRun(target, { report: undefined, prerequisites }).checks.at(-1)).toMatchObject({ name: "report", ok: false });

    // WebKit serialises undefined as null; the null renderer run has no gpu block.
    const webkit = judgeRun(target, { report: normalizeReport({ ...helloTtyReport(), gpu: null, detail: "ran 600 frames", frames: helloTtyReport().frames.map((frame) => ({ ...frame, gpu: null })) }), prerequisites, capabilities, frames: [] });
    expect(webkit.checks.find((check) => check.name === "report schema")).toMatchObject({ ok: true });
  });

  it("compares dump frames of the last replay against the native WebGPU renders with the fixture thresholds", () => {
    const target = resolveDeviceTarget("gs_blend/frame00700", fixturesRoot);
    if (target.kind !== "dump") throw new Error("dump expected");
    const native = nativeFrames(target);
    const frames = native.map(({ dumpFrame, rgba }, index) => ({ index: 4 + index, gpu: { dumpFrame, dumpLoop: 0, width: rgba.width, height: rgba.height, frameHash: "h" }, hostTimings: { renderMs: 3 }, rgba }));
    const exact = judgeDumpFrames(target, frames);
    expect(exact.map((verdict) => [verdict.dumpFrame, verdict.ok, verdict.md5Match])).toEqual([[1, true, true], [2, true, true], [3, true, true], [4, true, true]]);
    expect(exact[0]!.rmse).toMatchObject({ ok: true, rmse: 0, closePixelFraction: 1, thresholds: { maxRmse: 1, minClosePixels: 0.99 } });

    // A different GPU rounds a few channels differently: no MD5 match, still within the thresholds.
    const nudged = frames.map((frame) => ({ ...frame, rgba: { ...frame.rgba, data: frame.rgba.data.map((value: number, offset: number) => (offset % 4 !== 3 && offset % 4000 === 0 ? Math.min(255, value + 2) : value)) } }));
    const close = judgeDumpFrames(target, nudged);
    expect(close.every((verdict) => verdict.ok && !verdict.md5Match)).toBe(true);
    expect(close[0]!.rmse!.rmse).toBeGreaterThan(0);
    expect(close[0]!.rmse!.rmse).toBeLessThan(1);

    const wrong = frames.map((frame) => ({ ...frame, rgba: { ...frame.rgba, data: frame.rgba.data.map((value: number, offset: number) => (offset % 4 === 0 ? 255 - value : value)) } }));
    const failed = judgeDumpFrames(target, wrong);
    expect(failed.every((verdict) => !verdict.ok && !verdict.md5Match)).toBe(true);
    expect(failed[0]!.reason).toMatch(/exceed the thresholds/);

    const missing = judgeDumpFrames(target, frames.map((frame) => ({ ...frame, gpu: { ...frame.gpu, dumpLoop: 1 } })));
    expect(missing.every((verdict) => verdict.reason?.startsWith("not captured in the last replay"))).toBe(true);

    const run = judgeRun(target, { report: dumpReport(target).report, prerequisites, capabilities, frames });
    expect(run.passed, JSON.stringify(run.checks.filter((check) => !check.ok))).toBe(true);
    expect(run.checks.find((check) => check.name.startsWith("adapter"))).toMatchObject({ ok: true });
    const nvidia = judgeRun(target, { report: { ...dumpReport(target).report, gpu: { adapter: "WebGPU (WebGPU)\n0x2231\nNVIDIA RTX A5000", gsHost: "worker" } }, prerequisites, capabilities: { ...capabilities, adapter: undefined }, frames });
    expect(nvidia.checks.find((check) => check.name.startsWith("adapter"))).toMatchObject({ ok: false });
    const mainHost = judgeRun(target, { report: { ...dumpReport(target, "main").report }, prerequisites, capabilities: { ...capabilities, gpuInWorker: false }, frames });
    expect(mainHost.checks.find((check) => check.name.startsWith("navigator.gpu"))).toMatchObject({ name: "navigator.gpu where the GS runs (main)", ok: true });
    const pending = judgeRun(target, { report: dumpReport(target).report, prerequisites, capabilities });
    expect(pending.frames).toBeUndefined();
    expect(pending.passed).toBe(true);
  });

  it("measures boot time and vsyncs per wall second from the frame records", () => {
    const measurements = measure(helloTtyReport(), 50_000);
    expect(measurements).toMatchObject({ wallMs: 50_000, moduleCreateMs: 800, bootMs: 1100, firstFrameMs: 1200, observedFrames: 600, requestedFrames: 600, heapBytes: 805306368, threads: { busy: 2, idle: 4, total: 6, busyAfterStop: 0 } });
    expect(measurements.vsyncsPerSecond).toBeCloseTo(1000 / 70, 5);
    expect(measurements.vsyncsPerSecondFromBoot).toBeCloseTo(600 / ((1200 + 599 * 70 - 1100) / 1000), 5);
    expect(measure(undefined, 1)).toEqual({ wallMs: 1 });
  });
});

describe("device etiquette", () => {
  it("proceeds only when the device is attached, its page list is stable and no other lane runs", async () => {
    const pages = [rpcs3Page];
    const quiet = await checkDeviceQuiet({ discover: async () => ({ device, pages }), processes: () => [], sleep: async () => {}, intervalMs: 1 });
    expect(quiet).toMatchObject({ ok: true, pages });
    expect(quiet.checks).toHaveLength(2);
    expect(quiet.checks[0]).toMatchObject({ device: { name: device.deviceName, osVersion: "26.5.2" }, pages: [rpcs3Page.url], processes: [] });

    expect(await checkDeviceQuiet({ discover: async () => ({ device: undefined, pages: [] }), processes: () => [], sleep: async () => {} })).toMatchObject({ ok: false, reason: expect.stringMatching(/no device attached/) });
    expect(await checkDeviceQuiet({ discover: async () => ({ device, pages }), processes: () => ["1234 node scripts/run-full-runtime-device.mjs"], sleep: async () => {} })).toMatchObject({ ok: false, reason: expect.stringMatching(/another device lane is running/) });
    let reading = 0;
    const changing = await checkDeviceQuiet({ discover: async () => ({ device, pages: reading++ === 0 ? pages : [...pages, { url: "https://example.com/" }] }), processes: () => [], sleep: async () => {}, intervalMs: 1 });
    expect(changing).toMatchObject({ ok: false, reason: expect.stringMatching(/page list changed between checks/) });
    // Safari extension background pages come and go on their own and are not the user's pages.
    reading = 0;
    const extension = { url: "safari-web-extension://6F1C1096/background/background.html", title: "", webSocketDebuggerUrl: "ws://127.0.0.1:9222/devtools/page/2" };
    const flicker = await checkDeviceQuiet({ discover: async () => ({ device, pages: reading++ === 0 ? pages : [...pages, extension] }), processes: () => [], sleep: async () => {}, intervalMs: 1 });
    expect(flicker).toMatchObject({ ok: true });
    expect(flicker.checks[1]!.pages).toEqual([rpcs3Page.url, extension.url]);
  });

  it("notices a page list change during a run, once, ignoring the lane's own pages", async () => {
    let listing = [rpcs3Page, { url: "https://pcsx2.appmana.com/runtime.html?device=5" }];
    const changes: unknown[] = [];
    const watchdog = await watchPages({ discover: async () => ({ pages: listing }), pollMs: 2, isOwn: (url) => url.startsWith("https://pcsx2.appmana.com/"), onChange: (change) => { changes.push(change); } });
    expect(watchdog.baseline).toEqual([rpcs3Page.url]);
    listing = [rpcs3Page, { url: "https://pcsx2.appmana.com/runtime.html?device=6" }, { url: "safari-web-extension://6F1C1096/background/background.html", title: "", webSocketDebuggerUrl: "ws://x" }];
    await new Promise((resolve) => setTimeout(resolve, 20));
    expect(changes).toEqual([]);
    listing = [{ url: "https://example.com/" }];
    await new Promise((resolve) => setTimeout(resolve, 30));
    expect(changes).toEqual([{ before: [rpcs3Page.url], after: ["https://example.com/"] }]);
    expect(watchdog.changed).toBeDefined();
    watchdog.stop();
  });
});

describe("injected lane", () => {
  it("uploads the build, drives the page API, reads the frames back, judges, writes evidence and removes the injection", async () => {
    const target = resolveDeviceTarget("gs_blend/frame00700", fixturesRoot);
    const { report, pixels } = dumpReport(target);
    let driverContext: Record<string, any> | undefined;
    const fake = fakeSession((expression, awaitPromise) => {
      if (expression.includes("crossOriginIsolated") && expression.includes("JSON.stringify")) return JSON.stringify(prerequisites);
      if (expression.includes("const removed")) return JSON.stringify({ stopped: true, elements: ["pcsx2-device-lane"], urls: 15, opfs: "never created" });
      // The driver embeds the probe, which also creates a Blob URL: match it first.
      if (awaitPromise && expression.includes("const probe")) {
        driverContext = JSON.parse(expression.slice(expression.lastIndexOf("], {") + 3, -1));
        return { capabilities, gsHost: "worker", wallMs: 4321, report, page: { url: rpcs3Page.url, title: "RPCS3", hardwareConcurrency: 8, userAgent: "Safari" } };
      }
      if (expression.includes("URL.createObjectURL")) return ["worker", "acceptance"];
      if (expression.includes("__pcsx2DeviceResult") && expression.includes(".frames.find(")) return Buffer.from(pixels.get(frameIndexOf(expression))!).toString("base64");
      return undefined;
    });
    const evidenceRoot = path.join(scratch, "evidence");
    const currentDir = path.join(scratch, "current");
    const startedAt = new Date("2026-09-03T05:00:00Z");
    const outcome = await runInjectedDevice({ target, distDir, evidenceRoot, currentDir, startedAt, connect: async () => fake.session, ...quietOptions });
    expect(outcome.skipped).toBe(false);
    expect(outcome.passed, JSON.stringify(outcome.verdict?.checks.filter((check) => !check.ok))).toBe(true);
    expect(outcome.evidenceDir).toBe(path.join(evidenceRoot, "injected-gs_blend-frame00700-20260903T050000Z"));
    expect(outcome.verdict?.frames?.map((verdict) => [verdict.dumpFrame, verdict.ok, verdict.md5Match])).toEqual([[1, true, true], [2, true, true], [3, true, true], [4, true, true]]);
    expect(outcome.measurements).toMatchObject({ wallMs: 4321, bootMs: 1400, adapter: appleAdapter, gsHost: "worker", captured: 8 });
    expect(outcome.cleanup).toEqual({ stopped: true, elements: ["pcsx2-device-lane"], urls: 15, opfs: "never created" });
    expect(fake.closedCount()).toBe(1);

    expect(driverContext).toMatchObject({ target: target.targetUrl, targetName: "frame00700.gs.zst", panelId: "pcsx2-device-lane", options: { render: true, renderer: "webgpu", loops: 2, pthreadPoolSize: 6 } });
    const uploads = fake.evaluations.filter((entry) => entry.expression.includes(".parts.push("));
    expect(uploads.length).toBeGreaterThan(0);
    const injection = fake.evaluations.find((entry) => entry.expression.includes("URL.createObjectURL"))!;
    expect(injection.expression).toContain('replaceAll("__DEVICE_MODULE_worker__", urls["worker"])');
    expect(injection.expression).toContain('replaceAll("__DEVICE_MODULE_kit_disc_images_browser_opfs_js__", urls["kit/disc-images/browser/opfs.js"])');
    expect(injection.expression.indexOf('urls["kit/disc-images/core/index.js"]')).toBeLessThan(injection.expression.indexOf('urls["worker"]'));
    const order = fake.evaluations.map((entry) => entry.expression);
    const driverAt = order.findIndex((expression) => expression.includes("const probe"));
    const cleanupAt = order.findIndex((expression) => expression.includes("const removed"));
    const frameReads = order.filter((expression) => expression.includes(".frames.find("));
    expect(frameReads).toHaveLength(8);
    expect(driverAt).toBeLessThan(order.indexOf(frameReads[0]!));
    expect(cleanupAt).toBe(order.length - 1);
    expect(INJECTED_DRIVER).toContain("import(urls.acceptance)");
    expect(CLEANUP_EXPRESSION).toContain("revokeObjectURL");

    const files = ["report.json", "verdict.json", "page.png", ...[1, 2, 3, 4].flatMap((frame) => [`gs_blend-frame00700-loop0-frame0000${frame}.png`, `gs_blend-frame00700-loop1-frame0000${frame}.png`])];
    for (const file of files) expect(existsSync(path.join(outcome.evidenceDir!, file)), file).toBe(true);
    const written = JSON.parse(readFileSync(path.join(outcome.evidenceDir!, "report.json"), "utf8"));
    expect(written).toMatchObject({ emulator: "pcsx2", passed: true, device: { name: device.deviceName }, prerequisites, injection: expect.stringContaining("Blob URLs") });
    expect(written.verdict.frames).toHaveLength(4);
    expect(written.result.report.frames[0].gpu.rgbaBase64).toBeUndefined();
    expect(written.modules.map((module: { name: string }) => module.name)).toContain("kit/disc-images/browser/opfs.js");
    const verdict = JSON.parse(readFileSync(path.join(outcome.evidenceDir!, "verdict.json"), "utf8"));
    expect(verdict).toMatchObject({ lane: "injected", passed: true, target: { kind: "dump", name: "gs_blend-frame00700" }, capabilities: { adapter: { vendor: "apple" } }, cleanup: { urls: 15 } });
    expect(verdict.pngs).toHaveLength(8);
    const summary = JSON.parse(readFileSync(path.join(currentDir, "injected-gs_blend-frame00700.json"), "utf8"));
    expect(summary).toMatchObject({ schema: 1, lane: "injected", passed: true, device: { name: device.deviceName, osVersion: "26.5.2" }, measurements: { gsHost: "worker" }, report: { frames: 8, ok: true }, evidenceDir: path.relative(path.resolve(), outcome.evidenceDir!) });
    expect(JSON.stringify(summary)).not.toContain("rgbaBase64");
    const decoded = PNG.sync.read(readFileSync(path.join(outcome.evidenceDir!, "gs_blend-frame00700-loop0-frame00001.png")));
    expect(decoded.width).toBe((report.frames[4]!.gpu as { width: number }).width);
  });

  it("stops before touching the device when the quiet check fails, and cleans up when the driver throws", async () => {
    const target = resolveDeviceTarget("gs_blend/frame00700", fixturesRoot);
    const skipped = await runInjectedDevice({ target, distDir, evidenceRoot: path.join(scratch, "e2"), currentDir: path.join(scratch, "c2"), ...quietOptions, discover: async () => ({ device: undefined, pages: [] }), connect: async () => { throw new Error("must not connect"); } });
    expect(skipped).toMatchObject({ skipped: true, passed: false, reason: expect.stringMatching(/no device attached/) });

    const fake = fakeSession((expression, awaitPromise) => {
      if (expression.includes("crossOriginIsolated") && expression.includes("JSON.stringify")) return JSON.stringify(prerequisites);
      if (expression.includes("const removed")) return JSON.stringify({ stopped: false, elements: [], urls: 3, opfs: "kept" });
      if (awaitPromise && expression.includes("const probe")) throw new Error("RangeError: Out of memory");
      if (expression.includes("URL.createObjectURL")) return [];
      return undefined;
    });
    await expect(runInjectedDevice({ target, distDir, evidenceRoot: path.join(scratch, "e3"), currentDir: path.join(scratch, "c3"), connect: async () => fake.session, ...quietOptions })).rejects.toThrow(/Out of memory/);
    expect(fake.evaluations.at(-1)!.expression).toContain("const removed");
    expect(fake.closedCount()).toBe(1);
  });
});

describe("origin lane", () => {
  it("imports the BIOS into the origin's storage, runs the page API, reads the report back and returns the page", async () => {
    const target = resolveDeviceTarget("hello_tty", fixturesRoot);
    const report = helloTtyReport();
    const bios = path.join(scratch, "origin-bios.bin");
    writeFileSync(bios, Buffer.alloc(4096, 1));
    const opened: string[] = [];
    let imported: string | undefined;
    const fake = fakeSession((expression, awaitPromise) => {
      if (expression === "Boolean(window.__pcsx2Storage)") return true;
      if (expression.includes("__pcsx2Storage.list()")) return JSON.stringify(null);
      if (expression.includes("__pcsx2Storage.importFile(")) { imported = expression; return JSON.stringify({ size: 4096, mountedPath: "/opfs/pcsx2/bios/origin-bios.bin" }); }
      if (expression.startsWith('Boolean(window["__pcsx2Runtime"]')) return true;
      if (expression.includes("crossOriginIsolated") && expression.includes("JSON.stringify")) return JSON.stringify({ ...prerequisites, url: "https://pcsx2.appmana.com/runtime.html?device=1" });
      if (awaitPromise && expression.includes("gpuInWorker")) return capabilities;
      if (awaitPromise && expression.includes('window["__pcsx2Runtime"].run(')) {
        return { ok: true, detail: report.detail, bootResult: 0, moduleCreateMs: 800, frames: 600, gpu: undefined, waitForPacketsMs: {}, renderMs: {}, captureMs: {}, workingSet: report.workingSet, shutdown: { stoppedCleanly: true, stopMs: 5, workingSet: report.shutdown.workingSet }, emu: report.emu };
      }
      if (expression.includes(".lastReport") && expression.includes("JSON.stringify")) return JSON.stringify(report);
      return undefined;
    });
    const evidenceRoot = path.join(scratch, "origin-evidence");
    const currentDir = path.join(scratch, "origin-current");
    const outcome = await runOriginDevice({
      target,
      origin: "https://pcsx2.example.invalid/",
      biosPath: bios,
      evidenceRoot,
      currentDir,
      startedAt: new Date("2026-09-03T06:00:00Z"),
      connect: async (options) => { opened.push(String(options.url)); return fake.session; },
      ...quietOptions,
    });
    expect(outcome.skipped).toBe(false);
    expect(outcome.passed, JSON.stringify(outcome.verdict?.checks.filter((check) => !check.ok))).toBe(true);
    expect(opened).toEqual(["https://pcsx2.example.invalid/storage.html", expect.stringMatching(/^https:\/\/pcsx2\.example\.invalid\/runtime\.html\?device=\d+$/)]);
    expect(outcome.bios).toEqual({ imported: true, stored: "pcsx2/bios/origin-bios.bin", size: 4096, mountedPath: "/opfs/pcsx2/bios/origin-bios.bin" });
    expect(imported).toContain('"pcsx2/bios/origin-bios.bin"');
    const run = fake.evaluations.find((entry) => entry.expression.includes('window["__pcsx2Runtime"].run('))!;
    expect(run.expression).toContain('"bios":"pcsx2/bios/origin-bios.bin"');
    expect(run.expression).toContain('"frames":600,"render":false');
    expect(outcome.verdict?.tty).toMatchObject({ ok: true });
    expect(outcome.verdict?.cpu).toMatchObject({ ok: true });
    expect(outcome.measurements?.vsyncsPerSecond).toBeCloseTo(1000 / 70, 5);
    expect(fake.evaluations.at(-1)!.expression).toBe(`location.assign(${JSON.stringify(rpcs3Page.url)})`);
    expect(fake.closedCount()).toBe(2);
    expect(existsSync(path.join(outcome.evidenceDir!, "verdict.json"))).toBe(true);
    const written = JSON.parse(readFileSync(path.join(outcome.evidenceDir!, "report.json"), "utf8"));
    expect(written).toMatchObject({ origin: "https://pcsx2.example.invalid/runtime.html", fixture: target.targetUrl, passed: true, bios: { imported: true } });
    const summary = JSON.parse(readFileSync(path.join(currentDir, "origin-hello_tty.json"), "utf8"));
    expect(summary).toMatchObject({ lane: "origin", passed: true, origin: "https://pcsx2.example.invalid/runtime.html", tty: { ok: true }, cpu: { ok: true }, report: { tty: expect.any(Array), emu: { cpuJsonl: "600 records" } } });
  });

  it("picks the GS host from the worker probe for rendered runs", async () => {
    const target = resolveDeviceTarget("gs_blend/frame00700", fixturesRoot);
    const { report, pixels } = dumpReport(target, "main");
    const fake = fakeSession((expression, awaitPromise) => {
      if (expression.startsWith('Boolean(window["__pcsx2Runtime"]')) return true;
      if (expression.includes("crossOriginIsolated") && expression.includes("JSON.stringify")) return JSON.stringify(prerequisites);
      if (awaitPromise && expression.includes("gpuInWorker")) return { ...capabilities, gpuInWorker: false };
      if (awaitPromise && expression.includes('window["__pcsx2Runtime"].run(')) return { ok: true, bootResult: 0, frames: 8, workingSet: report.workingSet, shutdown: report.shutdown, emu: report.emu, waitForPacketsMs: {}, renderMs: {}, captureMs: {} };
      if (expression.includes(".lastReport") && expression.includes("JSON.stringify")) return JSON.stringify(report);
      if (expression.includes(".lastReport") && expression.includes(".frames.find(")) return Buffer.from(pixels.get(frameIndexOf(expression))!).toString("base64");
      return undefined;
    });
    const outcome = await runOriginDevice({ target, origin: "https://pcsx2.example.invalid/", evidenceRoot: path.join(scratch, "origin-e2"), currentDir: path.join(scratch, "origin-c2"), connect: async () => fake.session, restorePage: false, ...quietOptions });
    expect(outcome.passed, JSON.stringify(outcome.verdict?.checks.filter((check) => !check.ok))).toBe(true);
    const run = fake.evaluations.find((entry) => entry.expression.includes('window["__pcsx2Runtime"].run('))!;
    expect(run.expression).toContain('"gsHost":"main"');
    expect(outcome.verdict?.frames).toHaveLength(4);
    expect(fake.evaluations.some((entry) => entry.expression.startsWith("location.assign("))).toBe(false);
  });
});
