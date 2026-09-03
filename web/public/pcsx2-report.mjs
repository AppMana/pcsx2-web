// Pure helpers shared by runtime-worker.mjs and the Vitest suite: host status
// codes, renderer ids, setting keys, the TTY byte stream decoder, and the
// report assembly in the kit's schema (report.js: schema 1 plus an emulator
// specific `emu` block).

// pcsx2_web_status() values (web/host/pcsx2_web_main.cpp WebHost::Status).
export const STATUS = Object.freeze({
  Uninitialized: -1,
  Idle: 0,
  Initializing: 1,
  Running: 2,
  Paused: 3,
  Stopping: 4,
  BootFailed: 5,
  CPUThreadFailed: 6,
});

const STATUS_NAMES = new Map(Object.entries(STATUS).map(([name, code]) => [code, name]));

/** @param {number} code */
export function statusName(code) {
  return STATUS_NAMES.get(code) ?? `status ${code}`;
}

// GSRendererType (pcsx2/Config.h).
export const RENDERERS = Object.freeze({
  auto: -1,
  dx11: 3,
  null: 11,
  ogl: 12,
  sw: 13,
  software: 13,
  vk: 14,
  dx12: 15,
  metal: 17,
  webgpu: 18,
});

/**
 * The EmuCore/GS/Renderer value for a run: render=false selects the null
 * renderer, render=true the WebGPU hardware renderer, and a named renderer
 * wins over both (the software renderer when nothing is named). A numeric
 * string or number is passed through for renderers this table does not name
 * yet.
 * @param {{ render?: boolean, renderer?: string | number }} options
 */
export function rendererId(options = {}) {
  if (options.render === false) return RENDERERS.null;
  const renderer = options.renderer;
  if (renderer === undefined || renderer === null || renderer === "") return options.render === true ? RENDERERS.webgpu : RENDERERS.sw;
  if (typeof renderer === "number") return renderer;
  if (/^-?\d+$/.test(renderer)) return Number(renderer);
  const id = RENDERERS[/** @type {keyof typeof RENDERERS} */ (renderer.toLowerCase())];
  if (id === undefined) throw new Error(`unknown renderer "${renderer}"`);
  return id;
}

/** GS dump targets replay through GSDumpReplayer and need no BIOS. */
export function isGsDumpTarget(target) {
  return /\.gs(\.xz|\.zst)?$/i.test(String(target));
}

/**
 * Turns bytes into base64 without building one giant binary string per call
 * (frames are a few MB; btoa on chunks keeps the argument size bounded).
 * @param {Uint8Array} bytes
 */
export function bytesToBase64(bytes) {
  const CHUNK = 0x8000;
  let binary = "";
  for (let offset = 0; offset < bytes.length; offset += CHUNK) {
    binary += String.fromCharCode.apply(null, /** @type {number[]} */ (/** @type {unknown} */ (bytes.subarray(offset, offset + CHUNK))));
  }
  return btoa(binary);
}

/**
 * "EmuCore/GS/Renderer" is section "EmuCore/GS", key "Renderer": the split
 * is at the last slash, the same convention the native oracle manifest uses.
 * @param {string} settingKey
 */
export function splitSettingKey(settingKey) {
  const at = settingKey.lastIndexOf("/");
  if (at <= 0 || at === settingKey.length - 1) throw new Error(`setting key "${settingKey}" must be Section/Key`);
  return { section: settingKey.slice(0, at), key: settingKey.slice(at + 1) };
}

/**
 * Settings that force the interpreter for every unit, as the native oracle
 * records them; the wasm build has no recompilers, so these are documentation
 * of the run rather than a behaviour switch.
 */
export const INTERPRETER_SETTINGS = Object.freeze({
  "EmuCore/CPU/Recompiler/EnableEE": "false",
  "EmuCore/CPU/Recompiler/EnableIOP": "false",
  "EmuCore/CPU/Recompiler/EnableVU0": "false",
  "EmuCore/CPU/Recompiler/EnableVU1": "false",
  "EmuCore/CPU/Recompiler/EnableEECache": "false",
});

/**
 * Turns the byte chunks that pcsx2_web_tty_read hands out into lines. The
 * chunk boundaries fall anywhere, including inside a multi byte sequence or a
 * line, so the decoder streams and the last partial line stays pending.
 */
export class TtyDecoder {
  constructor() {
    this.decoder = new TextDecoder("utf-8");
    /** @type {string[]} */
    this.lines = [];
    this.pending = "";
    this.bytes = 0;
  }

  /** @param {Uint8Array} chunk */
  push(chunk) {
    this.bytes += chunk.byteLength;
    this.pending += this.decoder.decode(chunk, { stream: true });
    this.splitPending();
  }

  /** Flushes the decoder; a trailing line without a newline becomes a line. */
  finish() {
    this.pending += this.decoder.decode();
    this.splitPending();
    if (this.pending.length) {
      this.lines.push(this.pending);
      this.pending = "";
    }
    return this.lines;
  }

  splitPending() {
    let at;
    while ((at = this.pending.indexOf("\n")) >= 0) {
      let line = this.pending.slice(0, at);
      if (line.endsWith("\r")) line = line.slice(0, -1);
      this.lines.push(line);
      this.pending = this.pending.slice(at + 1);
    }
  }
}

/**
 * @typedef {object} RunReportFields
 * @property {boolean} ok
 * @property {string} [detail]
 * @property {number} [bootResult]
 * @property {number} [moduleCreateMs]
 * @property {Array<Record<string, unknown>>} [frames]
 * @property {Record<string, unknown>} [gpu]
 * @property {unknown[]} [events]
 * @property {Record<string, number>} [workingSet]
 * @property {{ stoppedCleanly: boolean, stopMs?: number, detail?: string, workingSet?: Record<string, number> }} [shutdown]
 * @property {string[]} [tty]
 * @property {Record<string, unknown>} [emu]
 */

/**
 * The kit report (schema 1, emulator "pcsx2") with the TTY lines at the top
 * level and everything PCSX2 specific under `emu`.
 * @param {RunReportFields} fields
 */
export function createRunReport(fields) {
  return {
    schema: 1,
    emulator: "pcsx2",
    ok: fields.ok,
    detail: fields.detail,
    bootResult: fields.bootResult,
    moduleCreateMs: fields.moduleCreateMs,
    frames: fields.frames ?? [],
    gpu: fields.gpu,
    events: fields.events ?? [],
    workingSet: fields.workingSet,
    shutdown: fields.shutdown,
    tty: fields.tty ?? [],
    emu: fields.emu ?? {},
  };
}
