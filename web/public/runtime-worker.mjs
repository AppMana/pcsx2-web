// Owns the PCSX2 module: creates it, stages the BIOS and the ELF in MEMFS,
// applies settings, boots, drains host tasks and TTY on a timer, waits for
// frames through Atomics.waitAsync on the host frame counter, and reports in
// the kit's schema. Plain ESM loaded by URL; nothing here is bundled.
//
// Messages in:  boot { coreUrl, pthreadPoolSize, bios: { name, bytes }, elf: { name, bytes },
//                     render, renderer, settings, cpu, trace, frames, timeoutMs, pad }
//               stop, pad { port, state }, snapshot, export-input-trace
// Messages out: runtime-progress, runtime-result { report }, runtime-shutdown, runtime-fatal,
//               input-trace
import { INTERPRETER_SETTINGS, STATUS, TtyDecoder, createRunReport, rendererId, splitSettingKey, statusName } from "./pcsx2-report.mjs";

const scope = /** @type {DedicatedWorkerGlobalScope} */ (/** @type {unknown} */ (self));

/** @type {any} */
let module;
/** @type {string[]} */
let logs = [];
let bootStartedAt = 0;
let frameCounterAddress = 0;
let observedFrames = 0;
let stopRequested = false;
let padState = { digital1: 0, digital2: 0, leftX: 128, leftY: 128, rightX: 128, rightY: 128 };
/** @type {Array<Record<string, unknown>>} */
const inputTrace = [];
/** @type {unknown[]} */
let events = [];
let traceSupported = false;
/** @type {Uint8Array[]} */
let traceChunks = [];
const tty = new TtyDecoder();

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

function pump() {
  runHostTasks();
  drainTty();
  drainTrace();
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

function applyPad(port, state) {
  padState = {
    digital1: Number(state.digital1 ?? padState.digital1) >>> 0,
    digital2: Number(state.digital2 ?? padState.digital2) >>> 0,
    leftX: Math.max(0, Math.min(255, Number(state.leftX ?? padState.leftX))) >>> 0,
    leftY: Math.max(0, Math.min(255, Number(state.leftY ?? padState.leftY))) >>> 0,
    rightX: Math.max(0, Math.min(255, Number(state.rightX ?? padState.rightX))) >>> 0,
    rightY: Math.max(0, Math.min(255, Number(state.rightY ?? padState.rightY))) >>> 0,
  };
  inputTrace.push({ frame: frameCount(), port, ...padState });
  if (hasExport("pcsx2_web_set_pad")) {
    module.ccall("pcsx2_web_set_pad", null, ["number", "number", "number", "number", "number", "number", "number"],
      [port, padState.digital1, padState.digital2, padState.leftX, padState.leftY, padState.rightX, padState.rightY]);
  }
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
  return {
    stoppedCleanly: stopped && after === STATUS.Idle,
    stopMs: performance.now() - startedAt,
    detail: stopped ? `${statusName(before)} -> ${statusName(after)}` : `still ${statusName(after)} after ${timeoutMs} ms`,
    workingSet: workingSet(),
  };
}

async function boot(request) {
  bootStartedAt = performance.now();
  const frames = Number.isInteger(request.frames) ? Math.max(1, request.frames) : 1;
  const timeoutMs = Number.isFinite(request.timeoutMs) ? Math.max(1_000, request.timeoutMs) : 120_000;
  const deadline = bootStartedAt + timeoutMs;
  const traceOptions = { cpu: false, ramEvery: 0, tty: true, ...(request.trace ?? {}) };
  let bootResult;
  let moduleCreateMs = 0;
  let initResult;
  let stage = "create";
  /** @type {Array<{ index: number, elapsedMs: number }>} */
  const frameRecords = [];
  let ok = false;
  let failure;
  const elfPath = `/fixtures/${request.elf.name}`;
  const biosDir = "/pcsx2/bios";

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
    module.FS.mkdirTree(biosDir);
    module.FS.writeFile(`${biosDir}/${request.bios.name}`, new Uint8Array(request.bios.bytes));
    module.FS.mkdirTree("/fixtures");
    module.FS.writeFile(elfPath, new Uint8Array(request.elf.bytes));
    pushEvent({ type: "files-staged", bios: `${biosDir}/${request.bios.name}`, biosBytes: request.bios.bytes.byteLength, elf: elfPath, elfBytes: request.elf.bytes.byteLength });

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
    setSetting("EmuCore/GS/Renderer", String(rendererId({ render: request.render, renderer: request.renderer })));
    if (request.cpu === "interpreter") for (const [key, value] of Object.entries(INTERPRETER_SETTINGS)) setSetting(key, value);
    for (const [key, value] of Object.entries(request.settings ?? {})) setSetting(key, value);
    pump();

    stage = "trace";
    traceSupported = hasExport("pcsx2_web_trace_enable") && hasExport("pcsx2_web_trace_read");
    if (traceSupported && (traceOptions.cpu || traceOptions.ramEvery > 0)) {
      const result = module.ccall("pcsx2_web_trace_enable", "number", ["number", "number"], [traceOptions.cpu ? 1 : 0, traceOptions.ramEvery | 0]);
      if (result !== 0) throw new Error(`pcsx2_web_trace_enable returned ${result}`);
    }
    frameCounterAddress = module._pcsx2_web_frame_count_address() >>> 0;
    if (request.pad) applyPad(0, request.pad);

    stage = "boot";
    bootResult = module.ccall("pcsx2_web_boot", "number", ["string", "string"], [elfPath, elfPath]) | 0;
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
    while (observedFrames < frames && !stopRequested) {
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
      const code = status();
      if (code !== STATUS.Running && code !== STATUS.Paused && observedFrames < frames) {
        throw new Error(`VM left the running state at frame ${observedFrames}/${frames} (${statusName(code)})`);
      }
    }
    ok = observedFrames >= frames;
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
  const cpuJsonl = traceSupported ? traceText(frames) : undefined;

  const report = createRunReport({
    ok: ok && Boolean(shutdown.stoppedCleanly),
    detail: failure ?? (shutdown.stoppedCleanly ? `ran ${observedFrames} frames` : `ran ${observedFrames} frames; ${shutdown.detail}`),
    bootResult,
    moduleCreateMs,
    frames: frameRecords,
    events,
    workingSet: workingSet(),
    shutdown,
    tty: ttyLines,
    emu: {
      target: request.target,
      elf: elfPath,
      bios: `${biosDir}/${request.bios.name}`,
      initResult,
      status: module ? status() : STATUS.Uninitialized,
      statusName: statusName(module ? status() : STATUS.Uninitialized),
      fatal: fatalDetail,
      renderer: rendererId({ render: request.render, renderer: request.renderer }),
      requestedFrames: frames,
      observedFrames,
      trace: { supported: traceSupported, cpu: traceOptions.cpu, ramEvery: traceOptions.ramEvery, tty: traceOptions.tty },
      cpuJsonl,
      logs: logs.slice(-200),
      inputTrace: { schema: 1, entries: inputTrace, applied: inputTrace.length },
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
    case "pad":
      if (module) applyPad(message.port | 0, message.state ?? {});
      else inputTrace.push({ frame: 0, port: message.port | 0, ...(message.state ?? {}) });
      return;
    case "snapshot":
      if (module) progress("snapshot");
      return;
    case "export-input-trace":
      scope.postMessage({ type: "input-trace", schema: 1, entries: inputTrace, flipCounter: module ? frameCount() : 0, applied: inputTrace.length });
      return;
    default:
      return;
  }
});
