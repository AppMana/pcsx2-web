// The iPad lanes behind `yarn device:origin` and `yarn device:runtime`,
// written so the Vitest suite drives them through injectable connections:
// target resolution from the fixture tree, the run options each lane sends
// through window.__pcsx2Runtime.run, the Blob URL module manifest and page
// driver of the injected lane, the judges (TTY and cpu.jsonl against the
// native oracle, dump frames against the native WebGPU renders, ELF frames
// against the oracle PNGs, device prerequisites and adapter identity), the
// measurements the Phase E gate reads (vsyncs per wall second, boot time,
// working set, threads), device etiquette (the quiet check before any
// device action and the page list watchdog during a run), and the evidence
// writers (report.json, verdict.json, frame PNGs, the committed summary).
import { execFileSync } from "node:child_process";
import { existsSync, readdirSync, readFileSync } from "node:fs";
import { mkdir, writeFile } from "node:fs/promises";
import path from "node:path";
import { compareMd5, encodePng, pngMd5, pngRmse } from "@appmana-public/web-emulator-harness/compare";
import { applyRewrites, runInjectedLane, runOriginLane } from "@appmana-public/web-emulator-harness/device-lanes";
import { validateReport } from "@appmana-public/web-emulator-harness/report";
import { frameCompareFor } from "@appmana-public/web-emulator-harness/test-toml";
import { DEFAULT_DISCOVERY_URL, WebKitConnection, delay, findPage, isInspectablePage, openPage, uploadBytes, waitForExpression } from "@appmana-public/web-emulator-harness/webkit-inspector";
import { compareCpu, compareTty, discoverFixtures } from "../tests/support/fixtures.ts";

export const WEB_ROOT = path.resolve(import.meta.dirname, "..");
export const FIXTURES_ROOT = path.join(WEB_ROOT, "tests", "fixtures");
export const DEFAULT_ORIGIN = "https://pcsx2.appmana.com/";
export const DEFAULT_DIST = path.join(WEB_ROOT, "dist");
export const DEFAULT_EVIDENCE_ROOT = path.join(WEB_ROOT, "device-evidence");
export const DEFAULT_CURRENT_DIR = path.join(WEB_ROOT, "device-evidence-current");
// The injected lane needs an existing cross-origin-isolated HTTPS page; any
// appmana.com runtime qualifies.
export const DEFAULT_PAGE_MATCH = "appmana.com";
export const DEFAULT_PTHREAD_POOL_SIZE = 6;
export const DEFAULT_TIMEOUT_MS = 600_000;
export const DEFAULT_QUIET_MS = 120_000;
export const DEFAULT_ADAPTER_PATTERN = /apple/i;
export const API_GLOBAL = "__pcsx2Runtime";
export const PANEL_ID = "pcsx2-device-lane";
export const UPLOAD_SLOT = "__pcsx2DeviceUpload";
export const URLS_SLOT = "__pcsx2DeviceUrls";
const RESULT_SLOT = "__pcsx2DeviceResult";
const RUNTIME_SLOT = "__pcsx2DeviceRuntime";
const PREVIOUS_API_SLOT = "__pcsx2DevicePreviousRuntime";
const LANE_PROCESS_PATTERN = "run-.*device";

/**
 * @typedef {object} ElfTarget
 * @property {"elf"} kind
 * @property {string} name
 * @property {import("../tests/support/fixtures.ts").Fixture} fixture
 * @property {string} targetUrl page-relative URL of the ELF
 * @property {string} localPath
 * @property {number} frames
 */

/**
 * @typedef {object} DumpTarget
 * @property {"dump"} kind
 * @property {string} name
 * @property {import("../tests/support/fixtures.ts").Fixture} fixture
 * @property {string} dumpName
 * @property {string} targetUrl
 * @property {string} localPath
 * @property {Map<number, string>} nativeFrames dump frame number -> native WebGPU PNG
 * @property {import("@appmana-public/web-emulator-harness/test-toml").CompareSpec | undefined} compare
 */

/** @typedef {ElfTarget | DumpTarget} DeviceTarget */

/**
 * `hello_tty` names a fixture ELF; `gs_blend/frame00700` (or with its
 * `.gs.zst` suffix) names a GS dump under that fixture's expected/dumps.
 * @param {string} spec
 * @param {string} [fixturesRoot]
 * @returns {DeviceTarget}
 */
export function resolveDeviceTarget(spec, fixturesRoot = FIXTURES_ROOT) {
  const [fixtureName, ...rest] = String(spec).split("/");
  const fixture = discoverFixtures(fixturesRoot).find((entry) => entry.name === fixtureName);
  if (!fixture) throw new Error(`no fixture named ${JSON.stringify(fixtureName)} under ${fixturesRoot}`);
  if (fixture.kind !== "elf") throw new Error(`fixture ${fixture.name} boots a disc image from storage; the device lanes run ELF fixtures and GS dumps`);
  const relativeDir = path.relative(fixturesRoot, fixture.dir).split(path.sep).join("/");
  if (!rest.length) {
    return { kind: "elf", name: fixture.name, fixture, targetUrl: fixture.targetUrl, localPath: fixture.elfPath, frames: fixture.config.kit.frames };
  }
  if (rest.length !== 1) throw new Error(`target ${JSON.stringify(spec)} must be <fixture> or <fixture>/<dump>`);
  const dumpName = String(rest[0]).replace(/\.gs(\.xz|\.zst)?$/i, "");
  const dumpsDir = path.join(fixture.expectedDir, "dumps");
  const file = existsSync(dumpsDir) ? readdirSync(dumpsDir).sort().find((entry) => /\.gs(\.xz|\.zst)?$/i.test(entry) && entry.replace(/\.gs(\.xz|\.zst)?$/i, "") === dumpName) : undefined;
  if (!file) throw new Error(`no dump named ${JSON.stringify(dumpName)} under ${dumpsDir}`);
  const nativeDir = path.join(fixture.expectedDir, "webgpu-native", dumpName);
  /** @type {Map<number, string>} */
  const nativeFrames = new Map();
  if (existsSync(nativeDir)) {
    for (const png of readdirSync(nativeDir).sort()) {
      const match = new RegExp(`^${dumpName}_frame(\\d+)\\.png$`).exec(png);
      if (match) nativeFrames.set(Number(match[1]), path.join(nativeDir, png));
    }
  }
  return {
    kind: "dump",
    name: `${fixture.name}-${dumpName}`,
    fixture,
    dumpName,
    targetUrl: path.posix.join("tests/fixtures", relativeDir, "expected", "dumps", file),
    localPath: path.join(dumpsDir, file),
    nativeFrames,
    compare: frameCompareFor(fixture.config.kit, "webgpu"),
  };
}

/** Dump replays run twice like the desktop lane; the last replay is what the native runner wrote. */
export const DUMP_LOOPS = 2;

/**
 * The options one run sends through the page API. An ELF runs its fixture's
 * frame count with the null renderer unless `render` asks for WebGPU, in
 * which case the frames at the fixture's [compare.frames.webgpu] triggers
 * (and one vsync either side, like the desktop frame oracle) are read back
 * and the CPU trace stays off. A dump replays through WebGPU with every
 * frame read back. `gsHost` left undefined lets the lane decide from the
 * WebGPU-in-worker probe.
 * @param {DeviceTarget} target
 * @param {{ render?: boolean, gsHost?: "worker" | "main", pthreadPoolSize?: number, timeoutMs?: number, bios?: string }} [options]
 */
export function runOptionsFor(target, options = {}) {
  const pthreadPoolSize = options.pthreadPoolSize ?? DEFAULT_PTHREAD_POOL_SIZE;
  const timeoutMs = options.timeoutMs ?? DEFAULT_TIMEOUT_MS;
  if (target.kind === "dump") {
    return { frames: 100_000, render: true, renderer: "webgpu", gsHost: options.gsHost, readback: "async", captureRgba: true, captureEvery: 1, loops: DUMP_LOOPS, timeoutMs, pthreadPoolSize };
  }
  const config = target.fixture.config;
  const render = options.render === true;
  const base = {
    frames: target.frames,
    render,
    cpu: config.kit.cpu,
    bios: options.bios,
    trace: { cpu: config.trace.cpu && !render, ramEvery: render ? 0 : config.trace.ramEvery, tty: config.trace.tty },
    timeoutMs,
    pthreadPoolSize,
  };
  if (!render) return base;
  const triggers = (config.webgpuFrames?.trigger ?? []).map(Number).filter(Number.isFinite);
  const captureFrames = [...new Set(triggers.flatMap((frame) => [frame - 1, frame, frame + 1]))].filter((frame) => frame >= 0).sort((a, b) => a - b);
  return {
    ...base,
    frames: Math.max(target.frames, triggers.length ? Math.max(...triggers) + 2 : 0),
    renderer: "webgpu",
    gsHost: options.gsHost,
    readback: "async",
    captureRgba: true,
    captureEvery: 0,
    captureFrames,
  };
}

// Page-side: does a Worker see navigator.gpu, and what adapter does it get?
// The source of an async function; evaluate `(${WEBGPU_WORKER_PROBE})()`.
export const WEBGPU_WORKER_PROBE = `async () => {
  const source = 'self.onmessage = async () => {'
    + ' const r = { gpuInWorker: Boolean(self.navigator.gpu), offscreenCanvas: typeof OffscreenCanvas !== "undefined", sharedArrayBuffer: typeof SharedArrayBuffer === "function", crossOriginIsolated: self.crossOriginIsolated === true, hardwareConcurrency: navigator.hardwareConcurrency, adapter: null, features: [] };'
    + ' try { const adapter = self.navigator.gpu ? await self.navigator.gpu.requestAdapter() : null;'
    + '   if (adapter) { const info = adapter.info || {};'
    + '     r.adapter = { vendor: info.vendor || "", architecture: info.architecture || "", device: info.device || "", description: info.description || "", isFallbackAdapter: Boolean(adapter.isFallbackAdapter) };'
    + '     r.features = Array.from(adapter.features).sort();'
    + '     const limits = adapter.limits; r.limits = { maxBufferSize: limits.maxBufferSize, maxStorageBufferBindingSize: limits.maxStorageBufferBindingSize, maxUniformBufferBindingSize: limits.maxUniformBufferBindingSize, maxTextureDimension2D: limits.maxTextureDimension2D, maxTextureArrayLayers: limits.maxTextureArrayLayers, maxBindGroups: limits.maxBindGroups, maxColorAttachments: limits.maxColorAttachments, maxComputeWorkgroupStorageSize: limits.maxComputeWorkgroupStorageSize }; }'
    + ' } catch (error) { r.error = String(error); }'
    + ' postMessage(r); };';
  const url = URL.createObjectURL(new Blob([source], { type: "text/javascript" }));
  const worker = new Worker(url);
  try {
    const probed = await new Promise((resolve, reject) => {
      const timer = setTimeout(() => reject(new Error("WebGPU worker probe timed out")), 15000);
      worker.onmessage = (event) => { clearTimeout(timer); resolve(event.data); };
      worker.onerror = (event) => { clearTimeout(timer); reject(new Error(event.message || "WebGPU worker probe failed")); };
      worker.postMessage({});
    });
    return { ...probed, gpuOnMain: Boolean(navigator.gpu), userAgent: navigator.userAgent };
  } finally {
    worker.terminate();
    URL.revokeObjectURL(url);
  }
}`;

// Page-side: a report without its pixel payloads; frames that had one are
// marked so Node can pull them one at a time.
export const SLIM_REPORT = `(report) => report && ({ ...report, frames: (report.frames || []).map((frame) => frame && frame.gpu && frame.gpu.rgbaBase64 ? { ...frame, gpu: { ...frame.gpu, rgbaBase64: undefined, hasRgba: true } } : frame) })`;

/**
 * The injected lane's page driver: an async `(urls, context) => result`
 * evaluated by the kit once the modules are Blob URLs. It probes WebGPU in
 * a Worker, mounts a small panel, imports the uploaded runtime-acceptance
 * module (which installs window.__pcsx2Runtime), runs the target through
 * that page API with the uploaded core, wasm and BIOS, and returns the
 * report without pixels; the full report waits in globalThis for Node.
 */
export const INJECTED_DRIVER = `async (urls, context) => {
  const probe = ${WEBGPU_WORKER_PROBE};
  const slim = ${SLIM_REPORT};
  const { target, targetName, biosName, options, panelId } = context;
  document.getElementById(panelId)?.remove();
  const previousRuntime = globalThis[${JSON.stringify(RUNTIME_SLOT)}];
  if (previousRuntime && typeof previousRuntime.stop === "function") { try { await previousRuntime.stop(); } catch {} }
  if (!(${JSON.stringify(PREVIOUS_API_SLOT)} in globalThis)) globalThis[${JSON.stringify(PREVIOUS_API_SLOT)}] = window[${JSON.stringify(API_GLOBAL)}];
  const panel = document.createElement("section");
  panel.id = panelId;
  panel.style.cssText = "position:fixed;top:0;right:0;z-index:2147483647;background:#181818;color:#eee;padding:8px;font:11px ui-monospace,monospace;max-width:360px;max-height:70vh;overflow:auto;border:1px solid #555";
  panel.innerHTML = '<style>#' + panelId + ' canvas { width: 320px; height: 240px; display: block; }</style>'
    + '<div id="' + panelId + '-title">PCSX2 device lane · ' + targetName + '</div>'
    + '<div id="pcsx2-device-canvas-host"></div>'
    + '<pre id="pcsx2-device-status" style="margin:4px 0;white-space:pre-wrap">probing WebGPU</pre>';
  document.body.append(panel);
  const status = panel.querySelector("#pcsx2-device-status");
  let capabilities;
  try { capabilities = await probe(); } catch (error) { capabilities = { gpuInWorker: false, gpuOnMain: Boolean(navigator.gpu), error: String(error) }; }
  const gsHost = options.gsHost || (capabilities.gpuInWorker ? "worker" : "main");
  // The runtime mirrors memory cards and inis to pcsx2/ in this origin's
  // storage; the cleanup removes that directory only if the run created it.
  let opfsPcsx2Existed = null;
  try { const root = await navigator.storage.getDirectory(); await root.getDirectoryHandle("pcsx2"); opfsPcsx2Existed = true; }
  catch (error) { opfsPcsx2Existed = error && error.name === "NotFoundError" ? false : null; }
  globalThis[${JSON.stringify(RESULT_SLOT)}] = { opfsPcsx2Existed };
  status.textContent = "loading runtime";
  await import(urls.acceptance);
  const runtime = window[${JSON.stringify(API_GLOBAL)}];
  globalThis[${JSON.stringify(RUNTIME_SLOT)}] = runtime;
  const runOptions = { ...options, gsHost, coreUrl: urls.core, wasmUrl: urls.wasm, targetName, bios: urls.bios, biosName };
  const startedAt = performance.now();
  let report;
  try {
    report = await runtime.run(urls.target, runOptions);
  } catch (error) {
    report = { schema: 1, emulator: "pcsx2", ok: false, detail: String(error && error.message || error), frames: [], events: [], tty: [], emu: {} };
  }
  const wallMs = performance.now() - startedAt;
  globalThis[${JSON.stringify(RESULT_SLOT)}] = { report, opfsPcsx2Existed };
  status.textContent = (report.ok ? "ok" : "failed") + " · " + report.detail + " · " + Math.round(wallMs) + " ms";
  return {
    capabilities,
    gsHost,
    wallMs,
    report: slim(report),
    page: { url: location.href, title: document.title, hardwareConcurrency: navigator.hardwareConcurrency, userAgent: navigator.userAgent },
  };
}`;

// Page-side: everything the injection added leaves the page again (the
// runtime is stopped, the panel and canvas removed, every Blob URL revoked,
// the globals deleted, a pre-existing page API restored).
export const CLEANUP_EXPRESSION = `(async () => {
  const removed = { stopped: false, elements: [], urls: 0, opfs: "kept" };
  const runtime = globalThis[${JSON.stringify(RUNTIME_SLOT)}];
  if (runtime && typeof runtime.stop === "function") { try { await runtime.stop(); removed.stopped = true; } catch {} }
  if (globalThis[${JSON.stringify(RESULT_SLOT)}]?.opfsPcsx2Existed === false) {
    try { const root = await navigator.storage.getDirectory(); await root.removeEntry("pcsx2", { recursive: true }); removed.opfs = "removed pcsx2/"; }
    catch (error) { removed.opfs = error && error.name === "NotFoundError" ? "never created" : String(error); }
  }
  for (const id of [${JSON.stringify(PANEL_ID)}, "pcsx2-canvas"]) { const element = document.getElementById(id); if (element) { element.remove(); removed.elements.push(id); } }
  for (const url of globalThis[${JSON.stringify(`${URLS_SLOT}List`)}] || []) { URL.revokeObjectURL(url); removed.urls += 1; }
  if (window[${JSON.stringify(API_GLOBAL)}] === runtime) {
    const previous = globalThis[${JSON.stringify(PREVIOUS_API_SLOT)}];
    if (previous) window[${JSON.stringify(API_GLOBAL)}] = previous; else delete window[${JSON.stringify(API_GLOBAL)}];
  }
  for (const key of [${JSON.stringify(`${URLS_SLOT}List`)}, ${JSON.stringify(URLS_SLOT)}, ${JSON.stringify(UPLOAD_SLOT)}, ${JSON.stringify(RUNTIME_SLOT)}, ${JSON.stringify(RESULT_SLOT)}, ${JSON.stringify(PREVIOUS_API_SLOT)}]) delete globalThis[key];
  return JSON.stringify(removed);
})()`;

/**
 * The static import specifiers of a plain ESM module: `import ... from "x"`,
 * `export ... from "x"` and bare `import "x"`. Dynamic imports are runtime
 * URLs and stay as they are.
 * @param {string} text
 */
export function collectStaticImports(text) {
  const specifiers = new Set();
  const pattern = /(?:^|[\n;])\s*(?:import|export)\b[^;'"]*?\bfrom\s*["']([^"']+)["']|(?:^|[\n;])\s*import\s*["']([^"']+)["']/g;
  for (const match of text.matchAll(pattern)) specifiers.add(/** @type {string} */ (match[1] ?? match[2]));
  return [...specifiers];
}

/** @param {string} name */
const placeholderFor = (name) => `__DEVICE_MODULE_${name.replace(/[^A-Za-z0-9]/g, "_")}__`;

/**
 * Walks the static import graph of plain ESM entry modules under `distDir`
 * and returns every module in dependency order, its relative specifiers
 * replaced by placeholders the kit swaps for the Blob URL of the module
 * they name (a Blob URL module cannot resolve a relative import).
 * @param {string} distDir
 * @param {Array<{ name: string, relative: string }>} entries
 * @returns {Array<{ name: string, relative: string, text: string, rewrites: Array<{ from: string, toModule: string }> }>}
 */
export function collectModuleGraph(distDir, entries) {
  const read = (/** @type {string} */ relative) => {
    const file = path.join(distDir, relative);
    if (!existsSync(file)) throw new Error(`${file} is missing; build the runtime first (yarn build)`);
    return readFileSync(file, "utf8");
  };
  /** @type {Array<{ name: string, relative: string, text: string, rewrites: Array<{ from: string, toModule: string }> }>} */
  const modules = [];
  /** @type {Map<string, string>} */
  const names = new Map();
  const visiting = new Set();
  const visit = (/** @type {string} */ relative, /** @type {string} */ name) => {
    const known = names.get(relative);
    if (known) return known;
    if (visiting.has(relative)) throw new Error(`import cycle through ${relative}`);
    visiting.add(relative);
    let text = read(relative);
    /** @type {Array<{ from: string, toModule: string }>} */
    const rewrites = [];
    for (const specifier of collectStaticImports(text)) {
      if (!specifier.startsWith("./") && !specifier.startsWith("../")) throw new Error(`${relative} imports ${JSON.stringify(specifier)}, which is not a relative module`);
      const dependency = path.posix.normalize(path.posix.join(path.posix.dirname(relative), specifier));
      const dependencyName = visit(dependency, dependency);
      const placeholder = placeholderFor(dependencyName);
      text = applyRewrites(text, [{ from: `"${specifier}"`, to: `"${placeholder}"` }]);
      if (!rewrites.some((entry) => entry.from === placeholder)) rewrites.push({ from: placeholder, toModule: dependencyName });
    }
    visiting.delete(relative);
    names.set(relative, name);
    modules.push({ name, relative, text, rewrites });
    return name;
  };
  for (const entry of entries) visit(entry.relative, entry.name);
  return modules;
}

/**
 * The local build as the kit's module manifest: the page modules and every
 * plain ESM module they import statically (pcsx2-report.mjs, the kit's OPFS
 * helpers) with their imports pointed at each other's Blob URLs, the
 * runtime page's element ids moved into the injected panel, the Emscripten
 * glue with its pthread worker constructor pointed at its own Blob URL, the
 * wasm, the target bytes, and the BIOS for ELF targets.
 * @param {{ distDir?: string, target: DeviceTarget, biosPath?: string }} options
 * @returns {import("@appmana-public/web-emulator-harness/device-lanes").ModuleManifestEntry[]}
 */
export function buildInjectedModules({ distDir = DEFAULT_DIST, target, biosPath }) {
  const read = (/** @type {string} */ relative) => {
    const file = path.join(distDir, relative);
    if (!existsSync(file)) throw new Error(`${file} is missing; build the runtime first (yarn build)`);
    return readFileSync(file);
  };

  let core = read("core/pcsx2-web.mjs").toString("utf8");
  const pthreadConstructor = /new Worker\(new URL\("pcsx2-web\.mjs",\s*import\.meta\.url\),\s*\{/;
  if (!pthreadConstructor.test(core)) throw new Error("Emscripten pthread worker constructor changed; update the rewrite in device-lane-support.mjs");
  core = core.replace(pthreadConstructor, "new Worker(import.meta.url, {");

  const graph = collectModuleGraph(distDir, [{ name: "worker", relative: "runtime-worker.mjs" }, { name: "acceptance", relative: "runtime-acceptance.mjs" }]);
  const worker = graph.find((module) => module.name === "worker");
  const acceptance = graph.find((module) => module.name === "acceptance");
  if (!worker || !acceptance) throw new Error("the module graph lost an entry");
  for (const marker of ["request.coreUrl", "request.wasmUrl"]) if (!worker.text.includes(marker)) throw new Error(`runtime-worker.mjs no longer honours ${marker}`);
  for (const marker of ["options.coreUrl", "options.wasmUrl", "options.targetName", "options.biosName", "api.lastReport"]) {
    if (!acceptance.text.includes(marker)) throw new Error(`runtime-acceptance.mjs no longer honours ${marker}`);
  }
  // The page constructs its worker by URL string (not an import) and looks
  // up its status and canvas elements by id; both move into the panel.
  const workerPlaceholder = placeholderFor("worker");
  acceptance.text = applyRewrites(acceptance.text, [
    { from: '"./runtime-worker.mjs"', to: `"${workerPlaceholder}"` },
    { from: '"#result"', to: '"#pcsx2-device-result"' },
    { from: '"#status"', to: '"#pcsx2-device-status"' },
    { from: '"canvas-host"', to: '"pcsx2-device-canvas-host"' },
  ]);
  acceptance.rewrites.push({ from: workerPlaceholder, toModule: "worker" });

  /** @type {import("@appmana-public/web-emulator-harness/device-lanes").ModuleManifestEntry[]} */
  const modules = [
    ...graph.map((module) => ({ name: module.name, bytes: Buffer.from(module.text), mime: "text/javascript", rewrites: module.rewrites })),
    { name: "core", bytes: Buffer.from(core), mime: "text/javascript" },
    { name: "wasm", bytes: read("core/pcsx2-web.wasm"), mime: "application/wasm" },
    { name: "target", bytes: readFileSync(target.localPath), mime: "application/octet-stream" },
  ];
  if (target.kind === "elf") {
    if (!biosPath) throw new Error("PCSX2_BIOS must name the BIOS image the ELF fixtures boot with");
    modules.push({ name: "bios", bytes: readFileSync(biosPath), mime: "application/octet-stream" });
  }
  return modules;
}

/**
 * @typedef {object} CapturedFrame
 * @property {number} index
 * @property {Record<string, any>} gpu
 * @property {{ renderMs?: number }} [hostTimings]
 * @property {import("@appmana-public/web-emulator-harness/compare").RgbaFrame} rgba
 */

/**
 * Pulls the pixels of every frame the slim report marks, one evaluation
 * each, from the full report the page keeps at `reportExpression`.
 * @param {{ evaluate: (expression: string, awaitPromise?: boolean) => Promise<any> }} connection
 * @param {string} reportExpression
 * @param {{ frames?: Array<Record<string, any>> }} slimReport
 * @returns {Promise<CapturedFrame[]>}
 */
export async function readCapturedFrames(connection, reportExpression, slimReport) {
  /** @type {CapturedFrame[]} */
  const frames = [];
  for (const frame of slimReport.frames ?? []) {
    if (!frame?.gpu?.hasRgba) continue;
    const index = Number(frame.index);
    const base64 = await connection.evaluate(`(${reportExpression}).frames.find((frame) => frame.index === ${index})?.gpu?.rgbaBase64 ?? ""`);
    if (typeof base64 !== "string" || !base64) throw new Error(`frame ${index}: the page has no pixels for it`);
    const data = new Uint8Array(Buffer.from(base64, "base64"));
    const { width, height } = frame.gpu;
    if (data.byteLength !== width * height * 4) throw new Error(`frame ${index}: ${data.byteLength} bytes for ${width}x${height} RGBA`);
    frames.push({ index, gpu: frame.gpu, hostTimings: frame.hostTimings, rgba: { width, height, data, frameHash: frame.gpu.frameHash } });
  }
  return frames;
}

/**
 * WebKit returns `undefined` properties as `null` through Runtime.evaluate;
 * the report schema treats absent and null differently, so nulls on optional
 * fields are dropped again.
 * @param {Record<string, any> | undefined} report
 */
export function normalizeReport(report) {
  if (!report || typeof report !== "object") return report;
  for (const key of ["detail", "bootResult", "moduleCreateMs", "gpu", "workingSet", "shutdown"]) if (report[key] === null) delete report[key];
  if (Array.isArray(report.frames)) {
    for (const frame of report.frames) {
      if (!frame || typeof frame !== "object") continue;
      for (const key of ["gpu", "hostTimings", "captureMs", "elapsedMs"]) if (frame[key] === null) delete frame[key];
    }
  }
  return report;
}

/** @param {number} value */
const pad5 = (value) => String(value).padStart(5, "0");

/**
 * @param {DeviceTarget} target
 * @param {CapturedFrame} frame
 */
export function framePngName(target, frame) {
  if (target.kind === "dump") return `${target.name}-loop${frame.gpu.dumpLoop ?? 0}-frame${pad5(frame.gpu.dumpFrame ?? 0)}.png`;
  return `${target.name}-oracle${pad5(frame.gpu.oracleFrame ?? 0)}-present${frame.index}.png`;
}

/** @param {import("@appmana-public/web-emulator-harness/compare").RmseReport} rmse */
const rmseSummary = (rmse) => ({ ok: rmse.ok, rmse: rmse.rmse, mae: rmse.mae, psnr: rmse.psnr, maxChannelError: rmse.maxChannelError, exactPixelFraction: rmse.exactPixelFraction, closePixelFraction: rmse.closePixelFraction, pixelMaxErrorPercentiles: rmse.pixelMaxErrorPercentiles, thresholds: rmse.thresholds });

/**
 * One dump: every native WebGPU frame of the last replay must have been
 * captured and stay within the fixture's webgpu thresholds; an MD5 match
 * (same pixels as the native Dawn render) is recorded but not required
 * across GPU vendors.
 * @param {DumpTarget} target
 * @param {CapturedFrame[]} frames
 */
export function judgeDumpFrames(target, frames) {
  const compare = target.compare;
  const thresholds = compare?.mode === "rmse" ? { maxRmse: compare.max_rmse, minClosePixels: compare.min_close_pixels } : {};
  const lastLoop = frames.filter((frame) => (frame.gpu.dumpLoop ?? 0) === 0);
  const verdicts = [];
  for (const [dumpFrame, nativePng] of [...target.nativeFrames.entries()].sort((a, b) => a[0] - b[0])) {
    const frame = lastLoop.find((candidate) => candidate.gpu.dumpFrame === dumpFrame);
    if (!frame) {
      verdicts.push({ dumpFrame, ok: false, reason: `not captured in the last replay (captured: ${frames.map((f) => `${f.gpu.dumpLoop}/${f.gpu.dumpFrame}`).join(" ") || "none"})` });
      continue;
    }
    const native = new Uint8Array(readFileSync(nativePng));
    const expected = pngMd5(native);
    const md5 = compareMd5(expected.md5, frame.rgba);
    let rmse;
    let reason;
    try {
      rmse = pngRmse(native, frame.rgba, thresholds);
    } catch (error) {
      reason = error instanceof Error ? error.message : String(error);
    }
    const ok = compare?.mode === "md5" ? md5.ok : md5.ok || Boolean(rmse?.ok);
    if (!ok && !reason) reason = rmse ? `rmse ${rmse.rmse.toFixed(4)} close ${(rmse.closePixelFraction * 100).toFixed(2)}% exceed the thresholds` : "no comparison";
    verdicts.push({ dumpFrame, ok, reason, md5Match: md5.ok, md5: { expected: md5.expected, actual: md5.actual }, width: frame.rgba.width, height: frame.rgba.height, nativeWidth: expected.width, nativeHeight: expected.height, renderMs: frame.hostTimings?.renderMs, changedPixels: frame.gpu.changedPixels, png: framePngName(target, frame), rmse: rmse && rmseSummary(rmse) });
  }
  if (!target.nativeFrames.size) verdicts.push({ dumpFrame: -1, ok: false, reason: `no native baseline under expected/webgpu-native/${target.dumpName}; run web/scripts/webgpu-native-baseline.sh` });
  return verdicts;
}

/**
 * ELF frames at the [compare.frames.webgpu] triggers against the oracle's
 * PNGs (software renderer), best of the three presents around each trigger.
 * @param {ElfTarget} target
 * @param {CapturedFrame[]} frames
 */
export function judgeElfFrames(target, frames) {
  const compare = target.fixture.config.webgpuFrames;
  const verdicts = [];
  if (!compare) return verdicts;
  const thresholds = compare.mode === "rmse" ? { maxRmse: compare.max_rmse, minClosePixels: compare.min_close_pixels } : {};
  for (const trigger of (compare.trigger ?? []).map(Number).filter(Number.isFinite)) {
    const oraclePng = path.join(target.fixture.expectedDir, "frames", `frame${pad5(trigger)}.png`);
    if (!existsSync(oraclePng)) {
      verdicts.push({ trigger, ok: false, reason: `no oracle frame ${oraclePng}` });
      continue;
    }
    const native = new Uint8Array(readFileSync(oraclePng));
    const candidates = frames.filter((frame) => Math.abs(Number(frame.gpu.oracleFrame) - trigger) <= 1);
    if (!candidates.length) {
      verdicts.push({ trigger, ok: false, reason: `nothing captured near it (captured oracle frames: ${frames.map((frame) => frame.gpu.oracleFrame).join(" ") || "none"})` });
      continue;
    }
    let best;
    const candidatesOut = [];
    for (const frame of candidates) {
      try {
        const rmse = pngRmse(native, frame.rgba, thresholds);
        candidatesOut.push({ oracleFrame: frame.gpu.oracleFrame, present: frame.index, png: framePngName(target, frame), ...rmseSummary(rmse) });
        if (!best || rmse.rmse < best.rmse.rmse) best = { frame, rmse };
      } catch (error) {
        candidatesOut.push({ oracleFrame: frame.gpu.oracleFrame, present: frame.index, png: framePngName(target, frame), ok: false, reason: error instanceof Error ? error.message : String(error) });
      }
    }
    const md5 = best ? compareMd5(pngMd5(native).md5, best.frame.rgba) : undefined;
    verdicts.push({
      trigger,
      ok: Boolean(best?.rmse.ok),
      reason: best ? (best.rmse.ok ? undefined : `best rmse ${best.rmse.rmse.toFixed(4)} close ${(best.rmse.closePixelFraction * 100).toFixed(2)}% exceed the thresholds`) : "no comparable capture",
      best: best && { oracleFrame: best.frame.gpu.oracleFrame, present: best.frame.index, png: framePngName(target, best.frame), md5Match: md5?.ok, ...rmseSummary(best.rmse) },
      candidates: candidatesOut,
    });
  }
  return verdicts;
}

/**
 * @typedef {object} RunUnderJudgement
 * @property {Record<string, any> | undefined} report the run's report (pixels optional)
 * @property {import("@appmana-public/web-emulator-harness/device-lanes").Prerequisites | undefined} [prerequisites]
 * @property {Record<string, any> | undefined} [capabilities] WEBGPU_WORKER_PROBE result
 * @property {CapturedFrame[]} [frames] undefined while the pixels are not read yet; frame checks are then skipped
 * @property {string} [aborted] why the run was cut short (page list changed)
 * @property {RegExp} [adapterPattern]
 */

/**
 * The verdict of one device run: a list of named checks (all must hold),
 * the TTY and cpu.jsonl comparisons for ELF targets, and the frame
 * comparisons for rendered runs.
 * @param {DeviceTarget} target
 * @param {RunUnderJudgement} run
 */
export function judgeRun(target, run) {
  const { report, prerequisites, capabilities, frames, aborted } = run;
  const adapterPattern = run.adapterPattern ?? DEFAULT_ADAPTER_PATTERN;
  /** @type {Array<{ name: string, ok: boolean, detail?: unknown }>} */
  const checks = [];
  const check = (/** @type {string} */ name, /** @type {unknown} */ ok, /** @type {unknown} */ detail) => {
    checks.push({ name, ok: Boolean(ok), ...(detail !== undefined ? { detail } : {}) });
    return Boolean(ok);
  };
  check("page list unchanged during the run", !aborted, aborted);
  for (const name of /** @type {const} */ (["secureContext", "crossOriginIsolated", "sharedArrayBuffer"])) check(name, prerequisites?.[name]);
  if (!report) {
    check("report", false, "the run produced no report");
    return { passed: false, checks, tty: undefined, cpu: undefined, frames: undefined };
  }
  const validation = validateReport(report);
  check("report schema", validation.ok, validation.errors.length ? validation.errors.join("; ") : undefined);
  check("run ok", report.ok === true, report.detail);
  check("bootResult 0", report.bootResult === 0, report.bootResult);
  check("stoppedCleanly", report.shutdown?.stoppedCleanly === true, report.shutdown?.detail);
  if (report.gpu) {
    const gsHost = report.gpu.gsHost === "main" ? "main" : "worker";
    check(`navigator.gpu where the GS runs (${gsHost})`, gsHost === "main" ? prerequisites?.webGpu || capabilities?.gpuOnMain : capabilities?.gpuInWorker);
    const identity = [report.gpu.adapter, capabilities?.adapter ? Object.values(capabilities.adapter).filter((value) => typeof value === "string").join(" ") : undefined].filter(Boolean).join("\n");
    check(`adapter matches ${adapterPattern}`, adapterPattern.test(identity), identity || "no adapter identity");
  }
  let tty;
  let cpu;
  let frameVerdicts;
  if (target.kind === "elf") {
    const fixture = target.fixture;
    check("observed frames", Number(report.emu?.observedFrames) >= target.frames, `${report.emu?.observedFrames ?? 0}/${target.frames}`);
    if (fixture.expectedTtyPath) {
      const verdict = compareTty(readFileSync(fixture.expectedTtyPath, "utf8"), report.tty ?? [], fixture.config.ttyFilter);
      tty = { ok: verdict.ok, reason: verdict.reason, expectedLines: verdict.expectedLines, actualLines: verdict.actualLines, firstDivergence: verdict.firstDivergence };
      check("tty matches expected/tty.txt", verdict.ok, verdict.reason);
    }
    if (fixture.expectedCpuPath && report.emu?.trace?.cpu) {
      if (typeof report.emu.cpuJsonl === "string") {
        const verdict = compareCpu(readFileSync(fixture.expectedCpuPath, "utf8"), report.emu.cpuJsonl, target.frames);
        cpu = { ok: verdict.ok, reason: verdict.reason, expectedRecords: verdict.expectedRecords, actualRecords: verdict.actualRecords, firstDivergence: verdict.firstDivergence };
        check("cpu.jsonl matches expected/cpu.jsonl", verdict.ok, verdict.reason);
      } else {
        check("cpu.jsonl matches expected/cpu.jsonl", false, "the run exported no cpu.jsonl");
      }
    }
    if (report.gpu && frames) {
      frameVerdicts = judgeElfFrames(target, frames);
      for (const verdict of frameVerdicts) check(`frame ${verdict.trigger} within the webgpu thresholds`, verdict.ok, verdict.reason);
    }
  } else if (frames) {
    frameVerdicts = judgeDumpFrames(target, frames);
    for (const verdict of frameVerdicts) check(`dump frame ${verdict.dumpFrame} matches expected/webgpu-native`, verdict.ok, verdict.reason);
  }
  return { passed: checks.every((entry) => entry.ok), checks, tty, cpu, frames: frameVerdicts };
}

/**
 * What the Phase E gate reads off a run: time to a running VM, the
 * interpreter's vsyncs per wall second between the first and the last
 * observed frame (and from VM start), the working set and thread counts.
 * @param {Record<string, any> | undefined} report
 * @param {number} [wallMs] the page-side wall time of run()
 */
export function measure(report, wallMs) {
  if (!report) return { wallMs };
  const events = Array.isArray(report.events) ? report.events : [];
  const vmRunning = events.find((event) => event?.type === "vm-running" && Number.isFinite(event.bootMs));
  const frames = (report.frames ?? []).filter((frame) => Number.isFinite(frame?.elapsedMs) && Number.isFinite(frame?.index));
  const first = frames[0];
  const last = frames.at(-1);
  const seconds = (/** @type {number} */ ms) => ms / 1000;
  const vsyncsPerSecond = first && last && last.index > first.index && last.elapsedMs > first.elapsedMs ? (last.index - first.index) / seconds(last.elapsedMs - first.elapsedMs) : undefined;
  const vsyncsPerSecondFromBoot = last && vmRunning && last.elapsedMs > vmRunning.bootMs ? (last.index + 1) / seconds(last.elapsedMs - vmRunning.bootMs) : undefined;
  const workingSet = report.workingSet;
  const shutdownWorkingSet = report.shutdown?.workingSet;
  return {
    wallMs,
    moduleCreateMs: report.moduleCreateMs,
    bootMs: vmRunning?.bootMs,
    firstFrameMs: first?.elapsedMs,
    lastFrameMs: last?.elapsedMs,
    observedFrames: report.emu?.observedFrames ?? frames.length,
    requestedFrames: report.emu?.requestedFrames,
    vsyncsPerSecond,
    vsyncsPerSecondFromBoot,
    heapBytes: workingSet?.heapBytes,
    workingSet,
    shutdownWorkingSet,
    threads: workingSet && { busy: workingSet.poolBusy, idle: workingSet.poolIdle, total: workingSet.poolTotal, busyAfterStop: shutdownWorkingSet?.poolBusy },
    adapter: report.gpu?.adapter,
    gsHost: report.gpu?.gsHost,
    captured: report.gpu?.captured,
  };
}

/**
 * Discovery that tolerates an absent device: `device` is undefined and
 * `pages` empty when nothing is attached.
 * @param {{ discoveryURL?: string, fetch?: typeof fetch }} [options]
 * @returns {Promise<{ device: import("@appmana-public/web-emulator-harness/webkit-inspector").InspectorDevice | undefined, pages: import("@appmana-public/web-emulator-harness/webkit-inspector").InspectorPage[] }>}
 */
export async function listDevicePages(options = {}) {
  const fetchImpl = options.fetch ?? fetch;
  const discoveryURL = options.discoveryURL ?? process.env.WIP_DISCOVERY_URL ?? DEFAULT_DISCOVERY_URL;
  const readJson = async (/** @type {string} */ url) => {
    const response = await fetchImpl(url);
    if (!response.ok) throw new Error(`${url} returned ${response.status}`);
    return response.json();
  };
  const devices = await readJson(discoveryURL);
  if (!devices.length) return { device: undefined, pages: [] };
  if (devices.length !== 1) throw new Error(`Expected one attached device, found ${devices.length}`);
  return { device: devices[0], pages: await readJson(`http://${devices[0].url}/json`) };
}

/**
 * The pages that count as the device's own: WebKit also lists Safari
 * extension background pages and service workers, which come and go on
 * their own (the kit's isInspectablePage rule).
 * @template {{ url: string, title?: string, webSocketDebuggerUrl?: string }} T
 * @param {T[]} pages
 */
export function userPages(pages) {
  return pages.filter((page) => isInspectablePage({ title: page.title ?? "", url: page.url, webSocketDebuggerUrl: page.webSocketDebuggerUrl ?? "ws://" }));
}

/** @param {Array<{ url: string, title?: string, webSocketDebuggerUrl?: string }>} pages */
export function pagesFingerprint(pages) {
  return userPages(pages).map((page) => page.url).sort().join("\n");
}

/** Other device lane processes (`run-*-device`), excluding this one and shells quoting the pattern. */
export function listLaneProcesses() {
  let output = "";
  try {
    output = execFileSync("pgrep", ["-af", LANE_PROCESS_PATTERN], { encoding: "utf8" });
  } catch {
    return [];
  }
  return output.split("\n").filter(Boolean).filter((line) => {
    const pid = Number(line.split(" ")[0]);
    return pid !== process.pid && pid !== process.ppid && !line.includes("pgrep") && !/\b(zsh|bash|sh) -c\b/.test(line);
  });
}

/**
 * The shared-device rule before any device action: the page list must be
 * identical across `samples` readings `intervalMs` apart, and no other
 * device lane may be running.
 * @param {{ discover?: () => Promise<{ device: any, pages: Array<{ url: string }> }>, processes?: () => string[], samples?: number, intervalMs?: number, sleep?: (ms: number) => Promise<void>, log?: (line: string) => void }} [options]
 */
export async function checkDeviceQuiet(options = {}) {
  const discover = options.discover ?? (() => listDevicePages());
  const processes = options.processes ?? listLaneProcesses;
  const samples = options.samples ?? 2;
  const intervalMs = options.intervalMs ?? DEFAULT_QUIET_MS;
  const sleep = options.sleep ?? delay;
  /** @type {Array<{ at: string, device: { name: string, osVersion: string } | undefined, pages: string[], processes: string[] }>} */
  const checks = [];
  let previous;
  let pages = [];
  for (let sample = 0; sample < samples; sample += 1) {
    if (sample > 0) {
      options.log?.(`device quiet check: waiting ${intervalMs} ms before reading the page list again`);
      await sleep(intervalMs);
    }
    const at = new Date().toISOString();
    const found = await discover();
    const running = processes();
    pages = found.pages;
    checks.push({ at, device: found.device && { name: found.device.deviceName, osVersion: found.device.deviceOSVersion }, pages: found.pages.map((page) => page.url), processes: running });
    options.log?.(`device quiet check ${sample + 1}/${samples} at ${at}: ${found.device ? `${found.device.deviceName} ${found.device.deviceOSVersion}` : "no device"}; pages ${JSON.stringify(found.pages.map((page) => page.url))}; lanes ${JSON.stringify(running)}`);
    if (!found.device) return { ok: false, reason: "no device attached (discovery lists none)", checks, pages };
    if (running.length) return { ok: false, reason: `another device lane is running: ${running.join("; ")}`, checks, pages };
    const fingerprint = pagesFingerprint(found.pages);
    if (previous !== undefined && previous !== fingerprint) return { ok: false, reason: "the device's page list changed between checks", checks, pages };
    previous = fingerprint;
  }
  return { ok: true, reason: undefined, checks, pages };
}

/**
 * Polls the page list during a run; the first change (a page opened,
 * closed, or navigated by someone else) calls `onChange` once and stops.
 * @param {{ discover?: () => Promise<{ pages: Array<{ url: string }> }>, pollMs?: number, isOwn?: (url: string) => boolean, onChange: (change: { before: string[], after: string[] }) => Promise<void> | void, log?: (line: string) => void }} options
 */
export async function watchPages(options) {
  const discover = options.discover ?? (() => listDevicePages());
  const pollMs = options.pollMs ?? 5_000;
  const isOwn = options.isOwn ?? (() => false);
  const relevant = (/** @type {Array<{ url: string, title?: string, webSocketDebuggerUrl?: string }>} */ pages) => userPages(pages).map((page) => page.url).filter((url) => !isOwn(url)).sort();
  const before = relevant((await discover()).pages);
  let stopped = false;
  /** @type {{ before: string[], after: string[] } | undefined} */
  let changed;
  /** @type {ReturnType<typeof setTimeout> | undefined} */
  let timer;
  const tick = async () => {
    if (stopped) return;
    try {
      const after = relevant((await discover()).pages);
      if (after.join("\n") !== before.join("\n")) {
        changed = { before, after };
        stopped = true;
        options.log?.(`device page list changed during the run: ${JSON.stringify(before)} -> ${JSON.stringify(after)}`);
        await options.onChange(changed);
        return;
      }
    } catch (error) {
      options.log?.(`device page watchdog: discovery failed (${error instanceof Error ? error.message : String(error)}); watching on`);
    }
    if (!stopped) timer = setTimeout(tick, pollMs);
  };
  timer = setTimeout(tick, pollMs);
  return {
    baseline: before,
    stop: () => { stopped = true; clearTimeout(timer); },
    get changed() { return changed; },
  };
}

/**
 * A session whose connection ignores the kit's close so the lane's caller
 * can keep evaluating (pixels, cleanup) and close it once at the end.
 * @param {import("@appmana-public/web-emulator-harness/device-lanes").LaneSession} session
 */
export function deferredSession(session) {
  let closed = false;
  const connection = {
    evaluate: (/** @type {string} */ expression, /** @type {boolean | undefined} */ awaitPromise) => session.connection.evaluate(expression, awaitPromise),
    command: (/** @type {string} */ method, /** @type {Record<string, unknown> | undefined} */ params) => session.connection.command(method, params),
    snapshotPng: () => session.connection.snapshotPng(),
    close: () => {},
  };
  return {
    session: { device: session.device, page: session.page, connection },
    close: () => {
      if (closed) return;
      closed = true;
      session.connection.close();
    },
  };
}

/**
 * Connects to the device's current HTTPS page whose URL contains `match`,
 * without navigating.
 * @param {string} match
 */
export function connectMatchingPage(match) {
  return async (/** @type {{ discoveryURL?: string, timeoutMs?: number, log?: (line: string) => void }} */ options) => {
    const found = await findPage((page) => page.url.startsWith("https://") && page.url.includes(match), { discoveryURL: options.discoveryURL, timeoutMs: options.timeoutMs });
    const connection = await new WebKitConnection(found.page.webSocketDebuggerUrl, { timeoutMs: options.timeoutMs, log: options.log }).open();
    return { device: found.device, page: found.page, connection };
  };
}

/** Navigates the device's current page to `url` (the kit's origin flow). */
export const connectByNavigation = async (/** @type {{ url?: string, discoveryURL?: string, timeoutMs?: number, log?: (line: string) => void }} */ options) => {
  if (!options.url) throw new Error("the origin lane needs a URL to open");
  return openPage(options.url, { discoveryURL: options.discoveryURL, timeoutMs: options.timeoutMs, commandTimeoutMs: options.timeoutMs, log: options.log });
};

/** @param {Date} [date] */
export function evidenceTimestamp(date = new Date()) {
  return date.toISOString().replace(/[-:]/g, "").replace(/\.\d{3}Z$/, "Z");
}

/** Strips what the committed summary must not carry (pixels, logs, the full trace text). */
function slimForSummary(/** @type {Record<string, any> | undefined} */ report) {
  if (!report) return undefined;
  const { frames, events, emu, ...rest } = report;
  return {
    ...rest,
    frames: (frames ?? []).length,
    events: (events ?? []).length,
    emu: emu && { ...emu, cpuJsonl: typeof emu.cpuJsonl === "string" ? `${emu.cpuJsonl.split("\n").filter(Boolean).length} records` : undefined, logs: undefined, inputTrace: undefined, recordedInputs: undefined },
  };
}

/**
 * Writes the run's own verdict next to the kit's report.json, the frame
 * PNGs, folds the final verdict into report.json, and refreshes the
 * committed summary (JSON without images) under `currentDir`.
 * @param {{ lane: "origin" | "injected", target: DeviceTarget, evidenceDir: string, currentDir: string, evidence: Record<string, any>, report: Record<string, any> | undefined, frames: CapturedFrame[], verdict: ReturnType<typeof judgeRun>, measurements: ReturnType<typeof measure>, capabilities?: Record<string, any>, runOptions: Record<string, unknown>, cleanup?: unknown, quiet?: unknown }} input
 */
export async function writeRunEvidence(input) {
  const { evidenceDir, currentDir, target } = input;
  await mkdir(evidenceDir, { recursive: true });
  const pngs = [];
  for (const frame of input.frames) {
    const name = framePngName(target, frame);
    await writeFile(path.join(evidenceDir, name), encodePng(frame.rgba));
    pngs.push(name);
  }
  const verdict = { capturedAt: new Date().toISOString(), lane: input.lane, target: { kind: target.kind, name: target.name, targetUrl: target.targetUrl }, passed: input.verdict.passed, checks: input.verdict.checks, tty: input.verdict.tty, cpu: input.verdict.cpu, frames: input.verdict.frames, measurements: input.measurements, capabilities: input.capabilities, runOptions: input.runOptions, pngs, cleanup: input.cleanup };
  const verdictPath = path.join(evidenceDir, "verdict.json");
  await writeFile(verdictPath, `${JSON.stringify(verdict, null, 2)}\n`);
  const reportPath = path.join(evidenceDir, "report.json");
  const merged = { ...input.evidence, passed: input.verdict.passed, verdict: { checks: input.verdict.checks, tty: input.verdict.tty, cpu: input.verdict.cpu, frames: input.verdict.frames }, measurements: input.measurements };
  await writeFile(reportPath, `${JSON.stringify(merged, null, 2)}\n`);
  await mkdir(currentDir, { recursive: true });
  const summary = {
    schema: 1,
    capturedAt: verdict.capturedAt,
    lane: input.lane,
    emulator: "pcsx2",
    target: verdict.target,
    device: input.evidence.device,
    origin: input.evidence.origin,
    page: input.evidence.result?.page,
    prerequisites: input.evidence.prerequisites,
    capabilities: input.capabilities,
    runOptions: input.runOptions,
    measurements: input.measurements,
    passed: input.verdict.passed,
    checks: input.verdict.checks,
    tty: input.verdict.tty,
    cpu: input.verdict.cpu,
    frames: input.verdict.frames,
    report: slimForSummary(input.report),
    quiet: input.quiet,
    cleanup: input.cleanup,
    evidenceDir: path.relative(WEB_ROOT, evidenceDir),
  };
  const summaryPath = path.join(currentDir, `${input.lane}-${target.name}.json`);
  await writeFile(summaryPath, `${JSON.stringify(summary, null, 2)}\n`);
  return { verdictPath, reportPath, summaryPath, pngs };
}

/**
 * @typedef {object} LaneRunOptions
 * @property {DeviceTarget} target
 * @property {string} [evidenceRoot]
 * @property {string} [currentDir]
 * @property {boolean} [render] ELF targets: WebGPU instead of the null renderer
 * @property {"worker" | "main"} [gsHost]
 * @property {number} [pthreadPoolSize]
 * @property {number} [timeoutMs]
 * @property {string} [discoveryURL]
 * @property {RegExp} [adapterPattern]
 * @property {number} [quietMs] 0 skips the quiet check
 * @property {number} [quietSamples]
 * @property {() => Promise<{ device: any, pages: Array<{ url: string }> }>} [discover]
 * @property {() => string[]} [processes]
 * @property {(ms: number) => Promise<void>} [sleep]
 * @property {number} [watchPollMs]
 * @property {(options: { url?: string, discoveryURL?: string, timeoutMs?: number, log?: (line: string) => void }) => Promise<import("@appmana-public/web-emulator-harness/device-lanes").LaneSession>} [connect]
 * @property {(line: string) => void} [log]
 * @property {Date} [startedAt]
 */

/**
 * @param {LaneRunOptions} options
 * @param {"origin" | "injected"} lane
 */
function laneDefaults(options, lane) {
  const log = options.log ?? (() => {});
  const startedAt = options.startedAt ?? new Date();
  const evidenceDir = path.join(options.evidenceRoot ?? DEFAULT_EVIDENCE_ROOT, `${lane}-${options.target.name}-${evidenceTimestamp(startedAt)}`);
  const currentDir = options.currentDir ?? DEFAULT_CURRENT_DIR;
  const timeoutMs = options.timeoutMs ?? DEFAULT_TIMEOUT_MS;
  const discover = options.discover ?? (() => listDevicePages({ discoveryURL: options.discoveryURL }));
  return { log, evidenceDir, currentDir, timeoutMs, discover };
}

/**
 * Uploads the local build into the device's current cross-origin-isolated
 * page, runs the target there, judges it, writes the evidence, and removes
 * the injection again.
 * @param {LaneRunOptions & { distDir?: string, biosPath?: string, pageMatch?: string, modules?: import("@appmana-public/web-emulator-harness/device-lanes").ModuleManifestEntry[] }} options
 */
export async function runInjectedDevice(options) {
  const { target } = options;
  const { log, evidenceDir, currentDir, timeoutMs, discover } = laneDefaults(options, "injected");
  const quiet = await checkDeviceQuiet({ discover, processes: options.processes, sleep: options.sleep, intervalMs: options.quietMs, samples: options.quietSamples, log });
  if (!quiet.ok) return { skipped: true, passed: false, reason: quiet.reason, quiet };
  const modules = options.modules ?? buildInjectedModules({ distDir: options.distDir, target, biosPath: options.biosPath });
  const runOptions = runOptionsFor(target, { render: options.render, gsHost: options.gsHost, pthreadPoolSize: options.pthreadPoolSize, timeoutMs });
  const targetName = path.basename(target.localPath);
  const biosName = options.biosPath ? path.basename(options.biosPath) : undefined;
  const connect = options.connect ?? connectMatchingPage(options.pageMatch ?? DEFAULT_PAGE_MATCH);
  const opened = deferredSession(await connect({ discoveryURL: options.discoveryURL, timeoutMs, log }));
  const { session } = opened;
  log(`injecting into ${session.page.url} on ${session.device.deviceName} ${session.device.deviceOSVersion}`);
  /** @type {string | undefined} */
  let aborted;
  const watchdog = await watchPages({
    discover,
    pollMs: options.watchPollMs,
    log,
    onChange: async (change) => {
      aborted = `page list changed during the run: ${JSON.stringify(change.before)} -> ${JSON.stringify(change.after)}`;
      await session.connection.evaluate(`window[${JSON.stringify(API_GLOBAL)}]?.stop?.()`, true).catch(() => undefined);
    },
  });
  let cleanup;
  try {
    const outcome = await runInjectedLane({
      emulator: "pcsx2",
      evidenceDir,
      discoveryURL: options.discoveryURL,
      timeoutMs,
      log,
      requiredPrerequisites: ["secureContext", "crossOriginIsolated", "sharedArrayBuffer", "wasm"],
      uploadSlot: UPLOAD_SLOT,
      urlsSlot: URLS_SLOT,
      modules,
      driver: INJECTED_DRIVER,
      driverContext: { target: target.targetUrl, targetName, biosName, options: runOptions, panelId: PANEL_ID },
      connect: async () => session,
      judge: (result, { prerequisites }) => judgeRun(target, { report: normalizeReport(result?.report), prerequisites, capabilities: result?.capabilities, aborted, adapterPattern: options.adapterPattern }).passed,
    });
    const result = outcome.evidence.result ?? {};
    const report = normalizeReport(result.report);
    const frames = report ? await readCapturedFrames(session.connection, `globalThis[${JSON.stringify(RESULT_SLOT)}].report`, report) : [];
    const verdict = judgeRun(target, { report, prerequisites: outcome.evidence.prerequisites, capabilities: result.capabilities, frames, aborted, adapterPattern: options.adapterPattern });
    const measurements = measure(report, result.wallMs);
    cleanup = await session.connection.evaluate(CLEANUP_EXPRESSION, true).then((value) => (typeof value === "string" ? JSON.parse(value) : value)).catch((error) => ({ error: error instanceof Error ? error.message : String(error) }));
    log(`injection removed: ${JSON.stringify(cleanup)}`);
    const paths = await writeRunEvidence({ lane: "injected", target, evidenceDir, currentDir, evidence: outcome.evidence, report, frames, verdict, measurements, capabilities: result.capabilities, runOptions, cleanup, quiet: quiet.checks });
    return { skipped: false, passed: verdict.passed, verdict, measurements, capabilities: result.capabilities, evidence: outcome.evidence, evidenceDir, paths: { ...outcome.paths, ...paths }, cleanup, aborted };
  } finally {
    watchdog.stop();
    if (!cleanup) {
      await session.connection.evaluate(CLEANUP_EXPRESSION, true).then((value) => log(`injection removed: ${value}`)).catch((error) => log(`injection cleanup failed: ${error instanceof Error ? error.message : String(error)}`));
    }
    opened.close();
  }
}

/**
 * Makes sure the origin's storage holds the BIOS the ELF fixtures boot with
 * (pcsx2/bios/<name>), importing it through storage.html's page API when it
 * is missing or a different size.
 * @param {{ origin: string, biosPath: string, connect: NonNullable<LaneRunOptions["connect"]>, discoveryURL?: string, timeoutMs: number, log: (line: string) => void }} options
 */
export async function ensureOriginBios(options) {
  const name = path.basename(options.biosPath);
  const stored = `pcsx2/bios/${name}`;
  const bytes = readFileSync(options.biosPath);
  const opened = deferredSession(await options.connect({ url: new URL("storage.html", options.origin).href, discoveryURL: options.discoveryURL, timeoutMs: options.timeoutMs, log: options.log }));
  const { connection } = opened.session;
  try {
    await waitForExpression(connection, "Boolean(window.__pcsx2Storage)", { timeoutMs: options.timeoutMs, what: "window.__pcsx2Storage" });
    const existing = await connection.evaluate(`window.__pcsx2Storage.list().then((entries) => JSON.stringify(entries.find((entry) => entry.path === ${JSON.stringify(stored)}) ?? null))`, true);
    const entry = typeof existing === "string" ? JSON.parse(existing) : null;
    if (entry && entry.size === bytes.byteLength) {
      options.log(`origin storage already holds ${stored} (${entry.size} bytes)`);
      return { imported: false, stored, size: entry.size };
    }
    await connection.evaluate(`globalThis[${JSON.stringify(UPLOAD_SLOT)}] = Object.create(null)`);
    await uploadBytes(connection, UPLOAD_SLOT, "bios", new Uint8Array(bytes), "application/octet-stream", { log: options.log });
    const imported = await connection.evaluate(`(async () => {
      const upload = globalThis[${JSON.stringify(UPLOAD_SLOT)}].bios;
      const parts = upload.parts.map((part) => Uint8Array.from(atob(part), (character) => character.charCodeAt(0)));
      delete globalThis[${JSON.stringify(UPLOAD_SLOT)}];
      const file = new File(parts, ${JSON.stringify(name)}, { type: upload.mimeType });
      const result = await window.__pcsx2Storage.importFile(file, ${JSON.stringify(stored)});
      return JSON.stringify({ size: file.size, mountedPath: result?.mountedPath });
    })()`, true);
    const outcome = typeof imported === "string" ? JSON.parse(imported) : {};
    options.log(`imported ${stored} into the origin's storage (${outcome.size} bytes)`);
    return { imported: true, stored, ...outcome };
  } finally {
    opened.close();
  }
}

/**
 * Runs the target on the hosted origin through the kit's origin lane (the
 * page's own API, the page's own build), then reads the full report back,
 * judges it and writes the evidence.
 * @param {LaneRunOptions & { origin?: string, biosPath?: string, restorePage?: boolean }} options
 */
export async function runOriginDevice(options) {
  const { target } = options;
  const { log, evidenceDir, currentDir, timeoutMs, discover } = laneDefaults(options, "origin");
  const origin = new URL(options.origin ?? DEFAULT_ORIGIN).href;
  const runtimeURL = new URL("runtime.html", origin).href;
  const quiet = await checkDeviceQuiet({ discover, processes: options.processes, sleep: options.sleep, intervalMs: options.quietMs, samples: options.quietSamples, log });
  if (!quiet.ok) return { skipped: true, passed: false, reason: quiet.reason, quiet };
  const originalPage = quiet.pages.find((page) => isInspectablePage(page) && !page.url.startsWith(runtimeURL))?.url;
  const connect = options.connect ?? connectByNavigation;
  let bios;
  if (target.kind === "elf" && options.biosPath) {
    bios = await ensureOriginBios({ origin, biosPath: options.biosPath, connect, discoveryURL: options.discoveryURL, timeoutMs, log });
  }
  const runOptions = runOptionsFor(target, { render: options.render, gsHost: options.gsHost, pthreadPoolSize: options.pthreadPoolSize, timeoutMs, bios: bios?.stored });
  /** @type {ReturnType<typeof deferredSession> | undefined} */
  let opened;
  /** @type {Awaited<ReturnType<typeof watchPages>> | undefined} */
  let watchdog;
  /** @type {string | undefined} */
  let aborted;
  /** @type {Record<string, any> | undefined} */
  let capabilities;
  const isOwn = (/** @type {string} */ url) => url.startsWith(runtimeURL) || url.startsWith(new URL("storage.html", origin).href);
  try {
    const outcome = await runOriginLane({
      emulator: "pcsx2",
      apiGlobal: API_GLOBAL,
      runtimeURL,
      target: target.targetUrl,
      runOptions,
      evidenceDir,
      discoveryURL: options.discoveryURL,
      timeoutMs,
      log,
      // The lane serialises runOptions after connecting, so the probe made
      // while opening the page can still pick the GS host.
      connect: async (connectOptions) => {
        opened = deferredSession(await connect(connectOptions));
        log(`opened ${opened.session.page.url} on ${opened.session.device.deviceName} ${opened.session.device.deviceOSVersion}`);
        capabilities = await opened.session.connection.evaluate(`(${WEBGPU_WORKER_PROBE})()`, true).catch((error) => ({ gpuInWorker: false, error: error instanceof Error ? error.message : String(error) }));
        if (runOptions.render && !runOptions.gsHost) runOptions.gsHost = capabilities?.gpuInWorker ? "worker" : "main";
        watchdog = await watchPages({
          discover,
          pollMs: options.watchPollMs,
          isOwn,
          log,
          onChange: async (change) => {
            aborted = `page list changed during the run: ${JSON.stringify(change.before)} -> ${JSON.stringify(change.after)}`;
            await opened?.session.connection.evaluate(`window[${JSON.stringify(API_GLOBAL)}]?.stop?.()`, true).catch(() => undefined);
          },
        });
        return opened.session;
      },
      judge: (summary) => Boolean(summary.ok) && summary.bootResult === 0 && summary.shutdown?.stoppedCleanly === true && !aborted,
    });
    if (!opened) throw new Error("the origin lane never opened a session");
    const { connection } = opened.session;
    const reportExpression = `window[${JSON.stringify(API_GLOBAL)}].lastReport`;
    const slim = await connection.evaluate(`JSON.stringify((${SLIM_REPORT})(${reportExpression}) ?? null)`);
    const report = normalizeReport(typeof slim === "string" ? JSON.parse(slim) ?? undefined : undefined);
    const frames = report ? await readCapturedFrames(connection, reportExpression, report) : [];
    const verdict = judgeRun(target, { report, prerequisites: outcome.evidence.prerequisites, capabilities, frames, aborted, adapterPattern: options.adapterPattern });
    const measurements = measure(report, outcome.evidence.elapsedMs);
    const paths = await writeRunEvidence({ lane: "origin", target, evidenceDir, currentDir, evidence: { ...outcome.evidence, bios }, report, frames, verdict, measurements, capabilities, runOptions, quiet: quiet.checks });
    return { skipped: false, passed: verdict.passed, verdict, measurements, capabilities, evidence: outcome.evidence, evidenceDir, paths: { ...outcome.paths, ...paths }, aborted, bios };
  } finally {
    watchdog?.stop();
    if (opened) {
      if (options.restorePage !== false && originalPage) {
        log(`returning the device page to ${originalPage}`);
        await opened.session.connection.evaluate(`location.assign(${JSON.stringify(originalPage)})`).catch(() => undefined);
      }
      opened.close();
    }
  }
}
