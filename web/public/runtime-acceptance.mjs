// The page side of the kit contract (@appmana-public/web-emulator-harness
// src/contract.js): window.__pcsx2Runtime = { run, stop, setPad, snapshot,
// exportInputTrace }. The module itself lives in runtime-worker.mjs; this
// page fetches the BIOS from origin-private storage and the ELF from its
// fixture URL, hands both to the worker, and relays the report.
import { isGsDumpTarget } from "./pcsx2-report.mjs";
import { createAudioHost } from "./pcsx2-web-audio.mjs";

const BIOS_DIR = "pcsx2/bios";
const MOUNT_ROOT = "/opfs";
// The canvas the GS presents to. transferControlToOffscreen() is permanent,
// so every run gets a fresh element under the same id.
const CANVAS_ID = "pcsx2-canvas";
const CANVAS_SELECTOR = `#${CANVAS_ID}`;

/** @type {Worker | undefined} */
let activeWorker;
/** @type {Promise<any> | undefined} */
let active;
let currentFrame = 0;
let currentPad = { digital1: 0, digital2: 0, leftX: 128, leftY: 128, rightX: 128, rightY: 128 };
/** @type {Array<Record<string, unknown>>} */
let recordedInputs = [];
// One AudioContext per page: creating it needs a user gesture (or Chrome's
// --autoplay-policy=no-user-gesture-required), and it survives runs.
/** @type {AudioContext | undefined} */
let audioContext;

const resultElement = document.querySelector("#result");
const statusElement = document.querySelector("#status");

function showStatus(text) {
  if (statusElement) statusElement.textContent = text;
}

// "/opfs/pcsx2/bios/x.bin", "/pcsx2/bios/x.bin" and "pcsx2/bios/x.bin" all
// name the same stored file.
function storageRelative(path) {
  let relative = String(path).replaceAll("\\", "/");
  if (relative.startsWith(`${MOUNT_ROOT}/`)) relative = relative.slice(MOUNT_ROOT.length + 1);
  relative = relative.replace(/^\/+/, "");
  if (!relative || relative.split("/").some((part) => part === "..")) throw new Error(`invalid storage path "${path}"`);
  return relative;
}

async function storageDirectory(parts, create = false) {
  if (!navigator.storage?.getDirectory) throw new Error("origin-private file storage is unavailable");
  let directory = await navigator.storage.getDirectory();
  for (const part of parts) directory = await directory.getDirectoryHandle(part, { create });
  return directory;
}

async function readStoredFile(relativePath) {
  const parts = storageRelative(relativePath).split("/");
  const name = parts.pop();
  const directory = await storageDirectory(parts);
  const handle = await directory.getFileHandle(name);
  const file = await handle.getFile();
  return { name, bytes: await file.arrayBuffer() };
}

// The first .bin under pcsx2/bios (sorted by name) when no BIOS is named.
async function defaultBiosPath() {
  let directory;
  try {
    directory = await storageDirectory(BIOS_DIR.split("/"));
  } catch (error) {
    throw new Error(`no BIOS directory ${MOUNT_ROOT}/${BIOS_DIR} in origin-private storage; import one on storage.html (${error.name})`);
  }
  const names = [];
  for await (const [name, handle] of directory.entries()) if (handle.kind === "file" && /\.bin$/i.test(name)) names.push(name);
  names.sort();
  if (!names.length) throw new Error(`no .bin BIOS under ${MOUNT_ROOT}/${BIOS_DIR}; import one on storage.html`);
  return `${BIOS_DIR}/${names[0]}`;
}

async function fetchTarget(target) {
  const url = new URL(target, location.href);
  const response = await fetch(url, { cache: "no-store" });
  if (!response.ok) throw new Error(`fetch ${url.pathname} failed with HTTP ${response.status}`);
  const bytes = await response.arrayBuffer();
  const name = decodeURIComponent(url.pathname.split("/").pop() || "target.elf");
  return { name, bytes };
}

// Replaces the presentation canvas with a fresh element of the requested
// size and hands its OffscreenCanvas to the caller.
function takeCanvas(width, height) {
  const previous = document.getElementById(CANVAS_ID);
  const canvas = document.createElement("canvas");
  canvas.id = CANVAS_ID;
  canvas.width = width;
  canvas.height = height;
  canvas.setAttribute("aria-label", "PCSX2 output");
  if (previous) previous.replaceWith(canvas);
  else (document.getElementById("canvas-host") ?? document.body).appendChild(canvas);
  return canvas.transferControlToOffscreen();
}

// The SPU2 mixes at 48 kHz; the context is created at that rate so the
// worklet pulls frames one to one and the browser resamples to the device.
export async function ensureAudioContext() {
  if (!audioContext) audioContext = new AudioContext({ sampleRate: 48000, latencyHint: "interactive" });
  if (audioContext.state !== "running") {
    try { await audioContext.resume(); } catch {}
  }
  return audioContext;
}

// target: the URL of an ELF or a GS dump (.gs, .gs.xz, .gs.zst) relative to
// this page, for example "tests/fixtures/hello_tty/hello_tty.elf" (served
// from the repository's fixture tree). Dumps replay through GSDumpReplayer
// and need no BIOS. options: frames, render (true: WebGPU hardware renderer,
// false: null renderer), renderer ("webgpu" | "sw" | "null"), gsHost
// ("worker" | "main": where the GS pump runs), readback ("none" | "async"),
// captureRgba, captureEvery, captureFrames (oracle frame numbers to read
// back), loops (dump replays), bios (storage path of the
// BIOS file), settings (Section/Key -> value), cpu, trace { cpu, ramEvery,
// tty, audio }, timeoutMs (0: no deadline), pthreadPoolSize, coreUrl, pad,
// inputTrace (frame-indexed pad states applied through the host's schedule),
// audio (true: play the SPU2 output through an audio worklet on this page's
// AudioContext), canvasWidth, canvasHeight. frames 0 runs until stop().
function run(target = "tests/fixtures/hello_tty/hello_tty.elf", options = {}) {
  if (active) return active;
  activeWorker?.terminate();
  currentFrame = 0;
  recordedInputs = [];
  active = (async () => {
    const timeoutMs = options.timeoutMs === 0 ? Infinity : Number.isFinite(options.timeoutMs) ? Math.max(1_000, options.timeoutMs) : 120_000;
    const coreUrl = new URL(options.coreUrl ?? "./core/pcsx2-web.mjs", location.href).href;
    const audioHost = options.audio ? createAudioHost({ audioContext: await ensureAudioContext() }) : undefined;
    const isDump = isGsDumpTarget(target);
    showStatus(`loading ${target}`);
    const biosPath = options.bios ? storageRelative(options.bios) : isDump ? undefined : await defaultBiosPath();
    const [bios, elf] = await Promise.all([biosPath ? readStoredFile(biosPath) : undefined, fetchTarget(target)]);
    showStatus(`booting ${elf.name}${bios ? ` with ${bios.name}` : ""}`);
    const wantsCanvas = options.render === true || options.renderer === "webgpu";
    const canvasWidth = Number.isInteger(options.canvasWidth) ? options.canvasWidth : 640;
    const canvasHeight = Number.isInteger(options.canvasHeight) ? options.canvasHeight : 480;
    const canvas = wantsCanvas ? takeCanvas(canvasWidth, canvasHeight) : undefined;
    const worker = new Worker("./runtime-worker.mjs", { type: "module" });
    activeWorker = worker;
    if (audioHost) worker.postMessage({ type: "audio-attach", port: audioHost.port, scriptUrl: coreUrl }, [audioHost.port]);
    const events = [];
    const report = await new Promise((resolve, reject) => {
      const timeout = Number.isFinite(timeoutMs) ? setTimeout(() => {
        worker.terminate();
        reject(new Error(`PCSX2 runtime timed out after ${timeoutMs + 15_000} ms; events=${JSON.stringify(events.slice(-20))}`));
      }, timeoutMs + 15_000) : undefined;
      worker.addEventListener("message", (event) => {
        const message = event.data;
        if (message?.type === "runtime-progress") {
          if (events.length < 2000) events.push(message);
          currentFrame = message.frame ?? currentFrame;
          showStatus(`${message.statusName} · frame ${message.frame} · ${(message.elapsedMs / 1000).toFixed(1)} s`);
          return;
        }
        if (message?.type === "runtime-result") {
          clearTimeout(timeout);
          resolve(message.report);
          return;
        }
        if (message?.type === "runtime-fatal") {
          clearTimeout(timeout);
          reject(new Error(`PCSX2 runtime worker failed: ${message.detail}\ntty tail: ${JSON.stringify(message.tty?.slice(-20) ?? [])}\nlogs tail: ${JSON.stringify(message.logs?.slice(-20) ?? [])}`));
        }
      });
      worker.addEventListener("error", (event) => {
        clearTimeout(timeout);
        reject(new Error(`PCSX2 runtime worker error: ${event.message || ""} ${event.filename || ""}:${event.lineno || 0}`.trim()));
      }, { once: true });
      const transfer = [elf.bytes];
      if (bios) transfer.push(bios.bytes);
      if (canvas) transfer.push(canvas);
      worker.postMessage({
        type: "boot",
        target,
        isDump,
        coreUrl,
        pthreadPoolSize: options.pthreadPoolSize,
        bios,
        elf,
        render: options.render,
        renderer: options.renderer,
        gsHost: options.gsHost,
        readback: options.readback,
        captureRgba: options.captureRgba,
        captureEvery: options.captureEvery,
        captureFrames: options.captureFrames,
        loops: options.loops,
        canvas,
        canvasSelector: CANVAS_SELECTOR,
        canvasWidth,
        canvasHeight,
        settings: options.settings ?? {},
        cpu: options.cpu ?? "interpreter",
        trace: options.trace,
        frames: options.frames,
        timeoutMs: Number.isFinite(timeoutMs) ? timeoutMs : 0,
        progressIntervalMs: options.progressIntervalMs,
        pad: options.pad ?? currentPad,
        inputTrace: options.inputTrace,
        audio: Boolean(options.audio),
      }, transfer);
    }).finally(() => {
      if (activeWorker === worker) activeWorker = undefined;
      worker.terminate();
      audioHost?.close();
    });
    report.emu.recordedInputs = recordedInputs;
    if (audioHost) report.emu.audio = { ...(report.emu.audio ?? {}), page: audioHost.stats() };
    if (resultElement) resultElement.textContent = JSON.stringify(report, null, 2);
    showStatus(`${report.ok ? "ok" : "failed"} · ${report.detail}`);
    return report;
  })().finally(() => { active = undefined; });
  return active;
}

// Resolves with the worker's shutdown report once the VM stopped.
function stop() {
  const worker = activeWorker;
  activeWorker = undefined;
  if (!worker) return Promise.resolve(undefined);
  return new Promise((resolve) => {
    const timer = setTimeout(() => { worker.terminate(); resolve(undefined); }, 15_000);
    worker.addEventListener("message", (event) => {
      if (event.data?.type !== "runtime-shutdown") return;
      clearTimeout(timer);
      const { type: _type, ...shutdown } = event.data;
      resolve(shutdown);
    });
    worker.postMessage({ type: "stop" });
  });
}

// setPad(state) per the kit contract, or setPad(port, state) for the second
// controller port.
function setPad(portOrState, maybeState) {
  const port = typeof portOrState === "number" ? portOrState : Number(portOrState?.port ?? 0);
  const state = typeof portOrState === "number" ? (maybeState ?? {}) : (portOrState ?? {});
  currentPad = { ...currentPad, ...state };
  recordedInputs.push({ frame: currentFrame, port, ...currentPad });
  activeWorker?.postMessage({ type: "pad", port, state: currentPad });
}

// Asks the running worker for a working set snapshot; it arrives as a
// progress event in the report's event list.
function snapshot() {
  activeWorker?.postMessage({ type: "snapshot" });
}

function exportInputTrace() {
  const worker = activeWorker;
  if (!worker) return Promise.resolve({ schema: 1, entries: recordedInputs, applied: recordedInputs.length });
  return new Promise((resolve) => {
    const timer = setTimeout(() => resolve({ schema: 1, entries: recordedInputs, applied: recordedInputs.length }), 5_000);
    const onTrace = (event) => {
      if (event.data?.type !== "input-trace") return;
      clearTimeout(timer);
      worker.removeEventListener("message", onTrace);
      const { type: _type, ...trace } = event.data;
      resolve(trace);
    };
    worker.addEventListener("message", onTrace);
    worker.postMessage({ type: "export-input-trace" });
  });
}

window.__pcsx2Runtime = { run, stop, setPad, snapshot, exportInputTrace };
showStatus("idle");
