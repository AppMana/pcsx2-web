// Owns the PCSX2 module: creates it, stages the BIOS (and an ELF) in MEMFS,
// restores the persisted memory cards and inis from origin-private storage,
// applies settings, boots an ELF or a disc image, drains host tasks and TTY on
// a timer, waits for frames through Atomics.waitAsync on the host frame
// counter, writes the memory cards and inis back, and reports in the kit's
// schema. Plain ESM loaded by URL; nothing here is bundled.
//
// Disc images stay in origin-private storage: the core opens "/opfs/<path>"
// through its own OPFS thread (pcsx2/CDVD/OpfsFileReader.cpp), so nothing is
// copied into MEMFS for them.
//
// Messages in:  boot { coreUrl, pthreadPoolSize, bios?: { name, bytes }, elf?: { name, bytes },
//                     disc?: { path }, isDump,
//                     render, renderer, gsHost, readback, captureRgba, captureEvery, captureFrames, loops,
//                     canvas?: OffscreenCanvas, canvasSelector, canvasWidth, canvasHeight,
//                     settings, cpu, trace, frames (0 = until stop), timeoutMs (0 = none), pad,
//                     inputTrace, audio }
//               audio-attach { port: MessagePort, scriptUrl }, stop, pad { port, state }, snapshot,
//               export-input-trace
// Messages out: runtime-progress, runtime-result { report }, runtime-shutdown, runtime-fatal,
//               input-trace
import { AUDIO_STATE_NAMES, INTERPRETER_SETTINGS, RENDERERS, STATUS, TtyDecoder, bytesToBase64, createRunReport, expandPadSchedule, mergePadState, packPadButtons, padPressureBytes, readAudioStats, rendererId, splitSettingKey, splitTraceRecords, statusName } from "./pcsx2-report.mjs";
import { directoryFor, fileHandleAt, isNotFound, storageRoot, writeAll } from "./kit/disc-images/browser/opfs.js";

const scope = /** @type {DedicatedWorkerGlobalScope} */ (/** @type {unknown} */ (self));

/** @type {any} */
let module;
/** @type {string[]} */
let logs = [];
let bootStartedAt = 0;
let frameCounterAddress = 0;
let observedFrames = 0;
let stopRequested = false;
/** @type {Record<string, unknown> | undefined} */
let padState;
/** @type {Array<Record<string, unknown>>} */
const inputTrace = [];
let padScheduled = 0;
// Web Audio: the page's MessagePort and the module script URL the page loads
// into its AudioWorkletGlobalScope (web/public/pcsx2-web-audio.mjs).
/** @type {MessagePort | undefined} */
let audioPort;
/** @type {string | undefined} */
let audioScriptUrl;
let audioStatsAddress = 0;
/** @type {Record<string, unknown>} */
let audioReport = { requested: false };
/** @type {unknown[]} */
let events = [];
let traceSupported = false;
// pcsx2_web_set_frame_limit stops the VM at the requested vsync count on the
// CPU thread, exactly where a tracerunner recording ends; without it the VM
// runs until the stop request lands.
let frameLimitArmed = false;
/** @type {Uint8Array[]} */
let traceChunks = [];
const tty = new TtyDecoder();
// Frames the GS thread read back (pcsx2_web_frame_read), in present order.
/** @type {Array<{ frame: number, width: number, height: number, dumpFrame: number, dumpLoop: number, oracleFrame: number, renderMs: number, changedPixels: number, frameHash: string, rgba: Uint8Array }>} */
let captures = [];
let captureSupported = false;

// Small state PCSX2 keeps under its data root in MEMFS, which would vanish
// with the worker: memory cards (Folders/MemoryCards) and inis
// (Folders/Settings). Each directory is mirrored to pcsx2/<dir>/ in
// origin-private storage through the kit's OPFS helpers: restored whole before
// the host initialises, written back when the VM stops, and while it runs
// every PERSIST_INTERVAL_MS for files whose MEMFS timestamp moved (a memory
// card save lands in MEMFS through FileMemoryCard's fwrite; polling its mtime
// costs nothing in the core). Limits: whole files are copied, so a snapshot
// taken between two page writes of one save is written and then overwritten by
// the next pass; the final pass after shutdown is authoritative. The host
// keeps its settings in memory, so inis/ is empty unless the core writes
// something there. Savestates are not persisted.
const DATA_ROOT = "/pcsx2";
const PERSIST_DIRS = ["memcards", "inis"];
const PERSIST_INTERVAL_MS = 10_000;
/** @type {Map<string, number>} MEMFS path -> mtime (ms) at the last write-back */
const persistedStamps = new Map();
/** @type {{ restored: Array<{ path: string, bytes: number }>, saved: Array<{ path: string, bytes: number }>, passes: number, error?: string }} */
const persistence = { restored: [], saved: [], passes: 0 };

async function restorePersistentState() {
  let root;
  try {
    root = await storageRoot();
  } catch (error) {
    persistence.error = `restore skipped: ${error instanceof Error ? error.message : String(error)}`;
    return;
  }
  for (const dir of PERSIST_DIRS) {
    const memfsDir = `${DATA_ROOT}/${dir}`;
    module.FS.mkdirTree(memfsDir);
    let directory;
    try {
      directory = await directoryFor(root, ["pcsx2", dir], false);
    } catch (error) {
      if (isNotFound(error)) continue;
      throw error;
    }
    for await (const [name, handle] of directory.entries()) {
      if (handle.kind !== "file") continue;
      const bytes = new Uint8Array(await (await handle.getFile()).arrayBuffer());
      const target = `${memfsDir}/${name}`;
      module.FS.writeFile(target, bytes);
      persistedStamps.set(target, module.FS.stat(target).mtime.getTime());
      persistence.restored.push({ path: target, bytes: bytes.byteLength });
    }
  }
}

// Writes MEMFS files under the persisted directories to storage; with
// changedOnly, only those whose mtime moved since their last write-back.
async function savePersistentState(changedOnly) {
  const root = await storageRoot();
  const saved = [];
  for (const dir of PERSIST_DIRS) {
    const memfsDir = `${DATA_ROOT}/${dir}`;
    let names;
    try {
      names = module.FS.readdir(memfsDir).filter((name) => name !== "." && name !== "..");
    } catch {
      continue;
    }
    for (const name of names) {
      const source = `${memfsDir}/${name}`;
      const stat = module.FS.stat(source);
      if (!module.FS.isFile(stat.mode)) continue;
      const stamp = stat.mtime.getTime();
      if (changedOnly && persistedStamps.get(source) === stamp) continue;
      const bytes = module.FS.readFile(source);
      const handle = await fileHandleAt(root, `pcsx2/${dir}/${name}`, true);
      const access = await handle.createSyncAccessHandle();
      try {
        writeAll(access, bytes, 0);
        access.truncate(bytes.length);
        access.flush();
      } finally {
        access.close();
      }
      persistedStamps.set(source, stamp);
      saved.push({ path: `pcsx2/${dir}/${name}`, bytes: bytes.length });
    }
  }
  persistence.passes += 1;
  persistence.saved.push(...saved);
  return saved;
}

async function persistIfPossible(changedOnly) {
  if (!module) return [];
  try {
    return await savePersistentState(changedOnly);
  } catch (error) {
    persistence.error = `write-back failed: ${error instanceof Error ? error.message : String(error)}`;
    pushEvent({ type: "persist-failed", detail: detail(error) });
    return [];
  }
}

// The sync access handle mode the core obtained for the last disc image it
// opened from origin-private storage ("read-only" on Chrome).
function opfsHandleMode() {
  if (!hasExport("pcsx2_web_opfs_handle_mode")) return undefined;
  const pointer = module._malloc(32);
  try {
    const count = module._pcsx2_web_opfs_handle_mode(pointer, 32) | 0;
    return count > 0 ? module.UTF8ToString(pointer) : "";
  } finally {
    module._free(pointer);
  }
}

function detail(error) {
  if (error instanceof Error) return `${error.name}: ${error.message}\n${error.stack ?? ""}`;
  // A pthread's crash arrives as an ErrorEvent re-dispatched by the
  // Emscripten runtime; its own error is nested one level down.
  if (typeof ErrorEvent === "function" && error instanceof ErrorEvent) {
    const where = error.filename ? ` at ${error.filename}:${error.lineno}:${error.colno}` : "";
    return `${error.message || "worker error"}${where}${error.error ? `\n${detail(error.error)}` : ""}`;
  }
  return String(error);
}

// The first crash (an unreachable trap or abort on any thread) ends the run
// with a report instead of a rejection, so the TTY and status captured so
// far are kept.
/** @type {string | undefined} */
let fatalDetail;
function recordFatal(error) {
  fatalDetail ??= detail(error);
  pushEvent({ type: "fatal", detail: fatalDetail });
}

function recordLog(line) {
  const text = String(line);
  logs.push(text);
  if (logs.length > 2000) logs = logs.slice(-1000);
}

function pushEvent(event) {
  if (events.length < 4000) events.push({ elapsedMs: bootStartedAt ? performance.now() - bootStartedAt : 0, ...event });
}

scope.addEventListener("error", (event) => {
  event.preventDefault();
  recordFatal(event.error ?? event);
});
scope.addEventListener("unhandledrejection", (event) => {
  event.preventDefault();
  recordFatal(event.reason);
});

const hasExport = (name) => typeof module?.[`_${name}`] === "function";

function status() {
  return module._pcsx2_web_status() | 0;
}

// The CPU thread queues work for the module's main thread; this worker is
// that thread, so it must drain the queue whenever it yields.
function runHostTasks() {
  return module._pcsx2_web_run_host_tasks() | 0;
}

function drainTty() {
  const pending = module._pcsx2_web_tty_pending() | 0;
  if (pending <= 0) return 0;
  const pointer = module._malloc(pending);
  try {
    const count = module._pcsx2_web_tty_read(pointer, pending) | 0;
    if (count > 0) tty.push(module.HEAPU8.slice(pointer, pointer + count));
    return count;
  } finally {
    module._free(pointer);
  }
}

// pcsx2_web_trace_read(buffer, size) returns the next bytes of the per frame
// JSONL (the tracerunner's cpu.jsonl format); it is optional in this build.
const TRACE_CHUNK = 64 * 1024;
function drainTrace() {
  if (!traceSupported) return 0;
  let total = 0;
  const pointer = module._malloc(TRACE_CHUNK);
  try {
    for (;;) {
      const count = module._pcsx2_web_trace_read(pointer, TRACE_CHUNK) | 0;
      if (count <= 0) break;
      traceChunks.push(module.HEAPU8.slice(pointer, pointer + count));
      total += count;
      if (count < TRACE_CHUNK) break;
    }
  } finally {
    module._free(pointer);
  }
  return total;
}

// The VM keeps running until the stop request lands on the CPU thread, so a
// few vsyncs past the requested count can be traced; the oracle stops at its
// frame limit, so only records below `frames` are kept.
function traceText(frames) {
  if (!traceChunks.length) return "";
  const size = traceChunks.reduce((sum, chunk) => sum + chunk.byteLength, 0);
  const joined = new Uint8Array(size);
  let offset = 0;
  for (const chunk of traceChunks) {
    joined.set(chunk, offset);
    offset += chunk.byteLength;
  }
  const text = new TextDecoder().decode(joined);
  if (!Number.isInteger(frames)) return text;
  return text.split("\n").filter((line) => {
    if (!line) return false;
    const match = /^\{"frame":(\d+)/.exec(line);
    return !match || Number(match[1]) < frames;
  }).map((line) => `${line}\n`).join("");
}

function frameCount() {
  if (!frameCounterAddress) return 0;
  return Atomics.load(new Int32Array(module.HEAPU8.buffer), frameCounterAddress >>> 2) >>> 0;
}

// Sleep until the host frame counter leaves `seen` or `timeoutMs` passes.
// Host::BeginPresentFrame increments the counter without a futex wake today,
// so the wait is bounded and re-checked; a notify from C++ shortens it.
async function waitForFrame(seen, timeoutMs) {
  if (frameCounterAddress && typeof Atomics.waitAsync === "function") {
    const heap = new Int32Array(module.HEAPU8.buffer);
    const result = Atomics.waitAsync(heap, frameCounterAddress >>> 2, seen | 0, Math.max(1, timeoutMs));
    if (result.async) await result.value;
    return;
  }
  await new Promise((resolve) => setTimeout(resolve, Math.max(1, timeoutMs)));
}

// Header words of pcsx2_web_frame_read: frame, width, height, dump frame, dump loop, oracle frame,
// render us, changed pixels, hash low, hash high.
const FRAME_HEADER_WORDS = 10;
function drainFrames() {
  if (!captureSupported) return 0;
  let count = 0;
  for (;;) {
    const bytes = module._pcsx2_web_frame_ready() | 0;
    if (bytes <= 0) break;
    const header = module._malloc(FRAME_HEADER_WORDS * 4);
    const rgba = module._malloc(bytes);
    try {
      const copied = module._pcsx2_web_frame_read(header, FRAME_HEADER_WORDS, rgba, bytes) | 0;
      if (copied <= 0) break;
      const words = module.HEAPU32.slice(header >>> 2, (header >>> 2) + FRAME_HEADER_WORDS);
      const hash = ((BigInt(words[9]) << 32n) | BigInt(words[8])).toString(16).padStart(16, "0");
      captures.push({
        frame: words[0],
        width: words[1],
        height: words[2],
        dumpFrame: words[3],
        dumpLoop: words[4] | 0,
        oracleFrame: words[5],
        renderMs: words[6] / 1000,
        changedPixels: words[7],
        frameHash: hash,
        rgba: module.HEAPU8.slice(rgba, rgba + copied),
      });
      count += 1;
    } finally {
      module._free(header);
      module._free(rgba);
    }
  }
  return count;
}

function gsDriverInfo() {
  if (!hasExport("pcsx2_web_gs_driver_info")) return "";
  const size = module._pcsx2_web_gs_driver_info(0, 0) | 0;
  if (size <= 0) return "";
  const pointer = module._malloc(size + 1);
  try {
    module._pcsx2_web_gs_driver_info(pointer, size + 1);
    return module.UTF8ToString(pointer);
  } finally {
    module._free(pointer);
  }
}

// Registers the page's OffscreenCanvas under the selector both as the entry
// pthread_create transfers to the GS thread ("#id", spawnThread's lookup) and
// as the entry findCanvasEventTarget resolves on this thread ("id", for the
// main-thread pump). The shared block holds width, height and the owning
// pthread, as libpthread.js lays it out.
function registerCanvas(canvas, selector) {
  const id = selector.replace(/^#/, "");
  const shared = module._malloc(12);
  module.HEAP32[shared >>> 2] = canvas.width;
  module.HEAP32[(shared >>> 2) + 1] = canvas.height;
  module.HEAPU32[(shared >>> 2) + 2] = 0;
  const info = { offscreenCanvas: canvas, canvasSharedPtr: shared, id };
  module.GL.offscreenCanvases[selector] = info;
  module.GL.offscreenCanvases[id] = info;
}

function pump() {
  runHostTasks();
  drainTty();
  drainTrace();
  drainFrames();
}

function workingSet() {
  if (!module) return undefined;
  const busy = module.PThread?.pthreads ? Object.keys(module.PThread.pthreads).length : 0;
  const idle = module.PThread?.unusedWorkers?.length ?? 0;
  return {
    heapBytes: module.HEAPU8?.byteLength ?? 0,
    poolBusy: busy,
    poolIdle: idle,
    poolTotal: busy + idle,
    frameCount: frameCount(),
    ttyBytes: tty.bytes,
    traceBytes: traceChunks.reduce((sum, chunk) => sum + chunk.byteLength, 0),
  };
}

function progress(kind = "progress") {
  const record = { type: kind, status: status(), statusName: statusName(status()), frame: frameCount(), workingSet: workingSet() };
  pushEvent(record);
  scope.postMessage({ type: "runtime-progress", ...record, elapsedMs: performance.now() - bootStartedAt });
}

function setSetting(settingKey, value) {
  const { section, key } = splitSettingKey(settingKey);
  const result = module.ccall("pcsx2_web_set_setting", "number", ["string", "string", "string"], [section, key, value === null || value === undefined ? null : String(value)]);
  if (result !== 0) throw new Error(`pcsx2_web_set_setting(${section}, ${key}) returned ${result}`);
}

// The host takes the twelve pressure bytes through a pointer; one scratch
// block serves every call.
let pressurePointer = 0;
function writePressures(state) {
  pressurePointer ||= module._malloc(12);
  module.HEAPU8.set(padPressureBytes(state), pressurePointer);
  return pressurePointer;
}

// Live pad state: merged like setPad(), applied by the host at the next vsync.
function applyPad(port, state) {
  const merged = mergePadState(port === 0 ? padState : undefined, state);
  if (port === 0) padState = merged;
  inputTrace.push({ frame: frameCount(), port, ...merged });
  if (!hasExport("pcsx2_web_set_pad")) return;
  const result = module._pcsx2_web_set_pad(port, packPadButtons(merged), merged.leftX, merged.leftY, merged.rightX, merged.rightY, writePressures(merged)) | 0;
  if (result !== 0) throw new Error(`pcsx2_web_set_pad returned ${result}`);
}

// Frame-indexed entries become the host's pad schedule, applied at the vsync
// with that frame count where a .p2m2 replay would override the pad.
function schedulePad(entries) {
  if (!hasExport("pcsx2_web_pad_schedule_add")) throw new Error("this build has no pad schedule exports");
  module._pcsx2_web_pad_schedule_clear();
  for (const state of expandPadSchedule(entries)) {
    const result = module._pcsx2_web_pad_schedule_add(state.frame, state.port, packPadButtons(state), state.leftX, state.leftY, state.rightX, state.rightY, writePressures(state)) | 0;
    if (result !== 0) throw new Error(`pcsx2_web_pad_schedule_add(${state.frame}) returned ${result}`);
    padScheduled += 1;
  }
}

function padAppliedCount() {
  return hasExport("pcsx2_web_pad_applied_count") ? module._pcsx2_web_pad_applied_count() | 0 : 0;
}

function audioStats() {
  if (!audioStatsAddress) return undefined;
  const stats = readAudioStats(new Uint32Array(module.HEAPU8.buffer), audioStatsAddress);
  return { ...stats, stateName: AUDIO_STATE_NAMES[String(stats.state)] ?? `state ${stats.state}` };
}

// Starts the audio worklet on the page's AudioContext and waits for the
// bootstrap (the page loads the module script into the worklet scope, which
// takes a moment) so the report can say whether audio ran.
async function attachAudio(coreUrl, timeoutMs) {
  const startedAt = performance.now();
  audioReport = { requested: true, attached: false };
  if (!hasExport("pcsx2_web_audio_attach")) {
    audioReport.detail = "this build has no audio exports";
    return;
  }
  if (!audioPort) {
    audioReport.detail = "the page sent no audio port";
    return;
  }
  audioStatsAddress = module._pcsx2_web_audio_stats_address() >>> 0;
  module.pcsx2AudioPort = audioPort;
  module.pcsx2AudioScriptUrl = audioScriptUrl ?? coreUrl;
  const result = module._pcsx2_web_audio_attach(48000) | 0;
  if (result !== 0) {
    audioReport.detail = `pcsx2_web_audio_attach returned ${result}`;
    return;
  }
  const deadline = startedAt + timeoutMs;
  for (;;) {
    pump();
    const state = module._pcsx2_web_audio_status() | 0;
    if (state === 2 || state === -1) {
      audioReport.attached = state === 2;
      audioReport.detail = state === 2 ? "worklet ready" : "worklet failed";
      break;
    }
    if (fatalDetail) throw new Error(fatalDetail);
    if (performance.now() >= deadline) {
      audioReport.detail = `worklet not ready after ${timeoutMs} ms`;
      break;
    }
    await new Promise((resolve) => setTimeout(resolve, 10));
  }
  audioReport.attachMs = performance.now() - startedAt;
}

/** Waits until `predicate(status)` holds, pumping the host queue; false on timeout. */
async function waitForStatus(predicate, timeoutMs) {
  const deadline = performance.now() + timeoutMs;
  for (;;) {
    pump();
    const current = status();
    if (predicate(current)) return true;
    if (fatalDetail) throw new Error(fatalDetail);
    if (performance.now() >= deadline) return false;
    await new Promise((resolve) => setTimeout(resolve, 5));
  }
}

async function stopVm(timeoutMs = 10_000) {
  const startedAt = performance.now();
  if (!module || status() === STATUS.Uninitialized) return { stoppedCleanly: false, detail: "module not initialized" };
  const before = status();
  if (fatalDetail) {
    pump();
    return { stoppedCleanly: false, stopMs: 0, detail: `not stopped: a thread crashed (${statusName(before)})`, workingSet: workingSet() };
  }
  if (before === STATUS.Running || before === STATUS.Paused || before === STATUS.Stopping) module._pcsx2_web_stop();
  let stopped = false;
  try {
    stopped = await waitForStatus((code) => code === STATUS.Idle || code === STATUS.BootFailed || code === STATUS.CPUThreadFailed, timeoutMs);
  } catch (error) {
    return { stoppedCleanly: false, stopMs: performance.now() - startedAt, detail: `crashed while stopping: ${error instanceof Error ? error.message : String(error)}`, workingSet: workingSet() };
  }
  pump();
  const after = status();
  // The VM closed its memory card files during shutdown; write every persisted file back.
  const persisted = await persistIfPossible(false);
  return {
    stoppedCleanly: stopped && after === STATUS.Idle,
    stopMs: performance.now() - startedAt,
    detail: stopped ? `${statusName(before)} -> ${statusName(after)}` : `still ${statusName(after)} after ${timeoutMs} ms`,
    workingSet: workingSet(),
    persisted,
  };
}

async function boot(request) {
  bootStartedAt = performance.now();
  // frames 0 runs until stop() (an interactive session); timeoutMs 0 removes the deadline.
  const unlimited = request.frames === 0;
  const frames = unlimited ? Infinity : Number.isInteger(request.frames) ? Math.max(1, request.frames) : 1;
  const timeoutMs = request.timeoutMs === 0 ? Infinity : Number.isFinite(request.timeoutMs) ? Math.max(1_000, request.timeoutMs) : 120_000;
  const deadline = bootStartedAt + timeoutMs;
  const traceOptions = { cpu: false, ramEvery: 0, tty: true, audio: false, ...(request.trace ?? {}) };
  let bootResult;
  let moduleCreateMs = 0;
  let initResult;
  let stage = "create";
  /** @type {Array<{ index: number, elapsedMs: number }>} */
  const frameRecords = [];
  let ok = false;
  let failure;
  const elfPath = request.elf ? `/fixtures/${request.elf.name}` : undefined;
  const discPath = request.disc?.path;
  const biosDir = "/pcsx2/bios";
  let lastPersist = bootStartedAt;
  const isDump = Boolean(request.isDump);
  const renderer = rendererId({ render: request.render, renderer: request.renderer });
  const readbackMode = request.readback === "async" ? 1 : 0;
  const captureFrames = Array.isArray(request.captureFrames) ? request.captureFrames.map((frame) => frame | 0) : [];
  const captureEvery = Number.isInteger(request.captureEvery) ? Math.max(0, request.captureEvery | 0) : request.captureRgba && !captureFrames.length ? 1 : 0;
  const wantsCapture = captureEvery > 0 || captureFrames.length > 0;
  // Dumps stop themselves after `loops` replays, the way the native runner does with -loop.
  let selfStopping = false;

  try {
    const coreUrl = request.coreUrl ? new URL(request.coreUrl, scope.location.href).href : new URL("./core/pcsx2-web.mjs", scope.location.href).href;
    const { default: createPCSX2 } = await import(coreUrl);
    module = await createPCSX2({
      pthreadPoolSize: Number.isInteger(request.pthreadPoolSize) ? request.pthreadPoolSize : 8,
      locateFile: (name) => new URL(name, coreUrl).href,
      print: recordLog,
      printErr: recordLog,
      onAbort: (reason) => { failure ??= `module aborted: ${reason}`; },
    });
    moduleCreateMs = performance.now() - bootStartedAt;
    pushEvent({ type: "module-created", moduleCreateMs, heapBytes: module.HEAPU8.byteLength });

    stage = "stage-files";
    if (!elfPath && !discPath) throw new Error("the boot request names neither an ELF nor a disc image");
    module.FS.mkdirTree(biosDir);
    if (request.bios) module.FS.writeFile(`${biosDir}/${request.bios.name}`, new Uint8Array(request.bios.bytes));
    if (elfPath) {
      module.FS.mkdirTree("/fixtures");
      module.FS.writeFile(elfPath, new Uint8Array(request.elf.bytes));
    }
    pushEvent({ type: "files-staged", bios: request.bios ? `${biosDir}/${request.bios.name}` : undefined, biosBytes: request.bios?.bytes.byteLength ?? 0, elf: elfPath, elfBytes: request.elf?.bytes.byteLength, disc: discPath });

    stage = "restore";
    await restorePersistentState();
    pushEvent({ type: "state-restored", restored: persistence.restored, error: persistence.error });

    stage = "gs";
    if (request.canvas) {
      if (!module.GL) throw new Error("the module does not export GL; OffscreenCanvas transfer needs -sOFFSCREENCANVAS_SUPPORT and GL in EXPORTED_RUNTIME_METHODS");
      registerCanvas(request.canvas, request.canvasSelector);
    }
    if (hasExport("pcsx2_web_set_gs_host")) {
      const result = module._pcsx2_web_set_gs_host(request.gsHost === "main" ? 1 : 0) | 0;
      if (result !== 0) throw new Error(`pcsx2_web_set_gs_host returned ${result}`);
    } else if (request.gsHost === "main") {
      throw new Error("this build has no pcsx2_web_set_gs_host export");
    }
    if (request.canvas) {
      const result = module.ccall("pcsx2_web_set_canvas", "number", ["string", "number", "number"], [request.canvasSelector, request.canvasWidth | 0, request.canvasHeight | 0]) | 0;
      if (result !== 0) throw new Error(`pcsx2_web_set_canvas returned ${result}`);
    }
    if (hasExport("pcsx2_web_set_readback_mode")) {
      const result = module._pcsx2_web_set_readback_mode(readbackMode) | 0;
      if (result !== 0) throw new Error(`pcsx2_web_set_readback_mode returned ${result}`);
    }
    captureSupported = hasExport("pcsx2_web_frame_ready") && hasExport("pcsx2_web_frame_read") && hasExport("pcsx2_web_set_frame_capture");
    if (wantsCapture) {
      if (!captureSupported) throw new Error("this build has no frame capture exports");
      if (readbackMode !== 1) throw new Error("frame capture needs readback: \"async\"");
      const result = module._pcsx2_web_set_frame_capture(captureEvery) | 0;
      if (result !== 0) throw new Error(`pcsx2_web_set_frame_capture returned ${result}`);
      for (const frame of captureFrames) {
        const added = module._pcsx2_web_add_capture_frame(frame) | 0;
        if (added !== 0) throw new Error(`pcsx2_web_add_capture_frame(${frame}) returned ${added}`);
      }
    }
    if (isDump) {
      if (!hasExport("pcsx2_web_set_dump_loop_count")) throw new Error("this build cannot replay GS dumps");
      const loops = Number.isInteger(request.loops) ? request.loops : 2;
      const result = module._pcsx2_web_set_dump_loop_count(loops) | 0;
      if (result !== 0) throw new Error(`pcsx2_web_set_dump_loop_count returned ${result}`);
      selfStopping = loops > 0;
    }

    stage = "init";
    initResult = module._pcsx2_web_init() | 0;
    if (initResult !== 0) throw new Error(`pcsx2_web_init returned ${initResult}`);
    if (!(await waitForStatus((code) => code !== STATUS.Initializing, Math.min(30_000, deadline - performance.now())))) {
      throw new Error(`host initialization did not finish (status ${statusName(status())})`);
    }
    if (status() === STATUS.CPUThreadFailed) throw new Error("CPU thread failed to start");
    pushEvent({ type: "initialized", status: status(), statusName: statusName(status()) });

    stage = "settings";
    setSetting("Folders/Bios", biosDir);
    setSetting("EmuCore/GS/Renderer", String(renderer));
    if (request.cpu === "interpreter") for (const [key, value] of Object.entries(INTERPRETER_SETTINGS)) setSetting(key, value);
    for (const [key, value] of Object.entries(request.settings ?? {})) setSetting(key, value);
    pump();

    stage = "trace";
    traceSupported = hasExport("pcsx2_web_trace_enable") && hasExport("pcsx2_web_trace_read");
    if (traceSupported && (traceOptions.cpu || traceOptions.ramEvery > 0 || traceOptions.audio)) {
      const mask = (traceOptions.cpu ? 1 : 0) | (traceOptions.audio ? 2 : 0);
      const result = module.ccall("pcsx2_web_trace_enable", "number", ["number", "number"], [mask, traceOptions.ramEvery | 0]);
      if (result !== 0) throw new Error(`pcsx2_web_trace_enable returned ${result}`);
    }
    frameCounterAddress = module._pcsx2_web_frame_count_address() >>> 0;
    if (hasExport("pcsx2_web_set_frame_limit") && !isDump && !unlimited) {
      const result = module._pcsx2_web_set_frame_limit(frames) | 0;
      if (result !== 0) throw new Error(`pcsx2_web_set_frame_limit returned ${result}`);
      frameLimitArmed = true;
    }

    stage = "pad";
    if (Array.isArray(request.inputTrace) && request.inputTrace.length) schedulePad(request.inputTrace);
    if (request.pad) applyPad(0, request.pad);

    stage = "audio";
    if (request.audio) await attachAudio(coreUrl, Math.min(30_000, deadline - performance.now()));

    stage = "boot";
    // An ELF boots as the ELF override of an empty CDVD; a disc image boots through the BIOS
    // and a GS dump replays without either.
    bootResult = module.ccall("pcsx2_web_boot", "number", ["string", "string"], [discPath ?? elfPath, discPath || isDump ? "" : elfPath]) | 0;
    if (bootResult !== 0) throw new Error(`pcsx2_web_boot returned ${bootResult}`);
    const started = await waitForStatus((code) => code === STATUS.Running || code === STATUS.Paused || code === STATUS.BootFailed || code === STATUS.CPUThreadFailed, deadline - performance.now());
    if (!started) throw new Error(`VM did not start within ${timeoutMs} ms (status ${statusName(status())})`);
    if (status() !== STATUS.Running && status() !== STATUS.Paused) throw new Error(`VM boot failed (${statusName(status())})`);
    pushEvent({ type: "vm-running", bootMs: performance.now() - bootStartedAt });
    progress("vm-running");

    stage = "run";
    let seen = frameCount();
    let lastProgress = performance.now();
    const progressEvery = Number.isFinite(request.progressIntervalMs) ? request.progressIntervalMs : 1000;
    while ((unlimited || observedFrames < frames) && !stopRequested) {
      if (fatalDetail) throw new Error(fatalDetail);
      const now = performance.now();
      if (now >= deadline) throw new Error(`frame ${observedFrames}/${frames} did not arrive within ${timeoutMs} ms`);
      await waitForFrame(seen, Math.min(20, deadline - now));
      pump();
      const current = frameCount();
      while (seen < current && observedFrames < frames) {
        seen += 1;
        observedFrames += 1;
        frameRecords.push({ index: observedFrames - 1, elapsedMs: performance.now() - bootStartedAt });
      }
      seen = current;
      if (performance.now() - lastProgress >= progressEvery) {
        lastProgress = performance.now();
        progress();
      }
      if (performance.now() - lastPersist >= PERSIST_INTERVAL_MS) {
        lastPersist = performance.now();
        await persistIfPossible(true);
      }
      const code = status();
      if ((frameLimitArmed || selfStopping) && (code === STATUS.Stopping || code === STATUS.Idle)) {
        // The host reached the frame limit; the remaining presents are flushed by the shutdown.
        if (code === STATUS.Idle) {
          pump();
          observedFrames = Math.max(observedFrames, Math.min(frames, frameCount()));
          break;
        }
        continue;
      }
      if (code !== STATUS.Running && code !== STATUS.Paused && observedFrames < frames) {
        throw new Error(`VM left the running state at frame ${observedFrames}/${frames} (${statusName(code)})`);
      }
    }
    if (frameLimitArmed && !stopRequested && observedFrames >= frames) {
      // Let the host stop on its own limit so the console output ends where the oracle's does.
      await waitForStatus((code) => code === STATUS.Idle || code === STATUS.BootFailed || code === STATUS.CPUThreadFailed, Math.max(1_000, deadline - performance.now()));
    }
    if (selfStopping && !stopRequested && status() !== STATUS.Idle) {
      await waitForStatus((code) => code === STATUS.Idle || code === STATUS.BootFailed || code === STATUS.CPUThreadFailed, Math.max(1_000, deadline - performance.now()));
    }
    ok = observedFrames >= frames || ((frameLimitArmed || selfStopping) && status() === STATUS.Idle) || (unlimited && stopRequested);
    if (!ok) failure = `stopped at frame ${observedFrames}/${frames}`;
  } catch (error) {
    failure = `${stage}: ${error instanceof Error ? error.message : String(error)}`;
    pushEvent({ type: "failure", stage, detail: detail(error) });
  }
  if (module) {
    try { pump(); } catch (error) { pushEvent({ type: "pump-failed", detail: detail(error) }); }
  }

  stage = "stop";
  let shutdown;
  try {
    shutdown = module ? await stopVm() : { stoppedCleanly: false, detail: "module was never created" };
  } catch (error) {
    shutdown = { stoppedCleanly: false, detail: detail(error) };
  }
  const ttyLines = tty.finish();
  const traceRecords = traceSupported ? splitTraceRecords(traceText(Number.isFinite(frames) ? frames : undefined)) : undefined;
  const cpuJsonl = traceRecords ? traceRecords.cpuJsonl : undefined;
  const audioJsonl = traceRecords && traceOptions.audio ? traceRecords.audioJsonl : undefined;

  // Captured frames attach to their frame record (index = host frame counter);
  // frames read back after the run loop ended get records of their own.
  /** @type {Array<Record<string, unknown>>} */
  const frames_out = frameRecords.map((record) => ({ ...record }));
  for (const capture of captures) {
    const gpu = {
      width: capture.width,
      height: capture.height,
      frameHash: capture.frameHash,
      changedPixels: capture.changedPixels,
      dumpFrame: capture.dumpFrame,
      dumpLoop: capture.dumpLoop,
      oracleFrame: capture.oracleFrame,
      rgbaBase64: request.captureRgba ? bytesToBase64(capture.rgba) : undefined,
    };
    const hostTimings = { renderMs: capture.renderMs };
    const record = frames_out.find((entry) => entry.index === capture.frame);
    if (record) Object.assign(record, { gpu, hostTimings });
    else frames_out.push({ index: capture.frame, gpu, hostTimings });
  }
  frames_out.sort((a, b) => Number(a.index) - Number(b.index));
  const adapter = module ? gsDriverInfo() : "";

  const report = createRunReport({
    ok: ok && Boolean(shutdown.stoppedCleanly),
    detail: failure ?? (shutdown.stoppedCleanly ? `ran ${observedFrames} frames` : `ran ${observedFrames} frames; ${shutdown.detail}`),
    bootResult,
    moduleCreateMs,
    frames: frames_out,
    gpu: renderer === RENDERERS.webgpu ? { adapter, captured: captures.length, readback: readbackMode ? "async" : "none", gsHost: request.gsHost === "main" ? "main" : "worker" } : undefined,
    events,
    workingSet: workingSet(),
    shutdown,
    tty: ttyLines,
    emu: {
      target: request.target,
      elf: elfPath,
      disc: discPath,
      opfsHandleMode: module && discPath ? opfsHandleMode() : undefined,
      persistence,
      bios: request.bios ? `${biosDir}/${request.bios.name}` : undefined,
      isDump,
      initResult,
      status: module ? status() : STATUS.Uninitialized,
      statusName: statusName(module ? status() : STATUS.Uninitialized),
      fatal: fatalDetail,
      renderer,
      requestedFrames: Number.isFinite(frames) ? frames : 0,
      observedFrames,
      trace: { supported: traceSupported, cpu: traceOptions.cpu, ramEvery: traceOptions.ramEvery, tty: traceOptions.tty, audio: traceOptions.audio },
      cpuJsonl,
      audioJsonl,
      audio: { ...audioReport, worklet: module ? audioStats() : undefined },
      logs: logs.slice(-200),
      inputTrace: { schema: 1, entries: inputTrace, applied: module ? padAppliedCount() : 0, scheduled: padScheduled },
    },
  });
  scope.postMessage({ type: "runtime-result", report });
}

scope.addEventListener("message", async (event) => {
  const message = event.data;
  switch (message?.type) {
    case "boot":
      try {
        await boot(message);
      } catch (error) {
        scope.postMessage({ type: "runtime-fatal", detail: detail(error), tty: tty.lines.slice(-200), logs: logs.slice(-200) });
      }
      return;
    case "stop": {
      stopRequested = true;
      const shutdown = await stopVm();
      scope.postMessage({ type: "runtime-shutdown", ...shutdown });
      return;
    }
    case "audio-attach":
      audioPort = message.port;
      audioScriptUrl = message.scriptUrl;
      return;
    case "pad":
      try {
        if (module) applyPad(message.port | 0, message.state ?? {});
        else inputTrace.push({ frame: 0, port: message.port | 0, ...(message.state ?? {}) });
      } catch (error) {
        pushEvent({ type: "pad-rejected", detail: detail(error) });
      }
      return;
    case "snapshot":
      if (module) progress("snapshot");
      return;
    case "export-input-trace":
      scope.postMessage({ type: "input-trace", schema: 1, entries: inputTrace, flipCounter: module ? frameCount() : 0, applied: module ? padAppliedCount() : 0, scheduled: padScheduled });
      return;
    default:
      return;
  }
});
