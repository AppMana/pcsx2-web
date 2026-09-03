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

// Pad states as the host takes them: the kit's DIGITAL1/DIGITAL2 bit layout
// (contract.js PadState) packed into one button word, plus the twelve
// pressure bytes in the .p2m2 order (p2m2.js) that pcsx2_web_set_pad and
// pcsx2_web_pad_schedule_add accept.
export const DIGITAL1 = Object.freeze({ select: 0x01, l3: 0x02, r3: 0x04, start: 0x08, up: 0x10, right: 0x20, down: 0x40, left: 0x80 });
export const DIGITAL2 = Object.freeze({ l2: 0x01, r2: 0x02, l1: 0x04, r1: 0x08, triangle: 0x10, circle: 0x20, cross: 0x40, square: 0x80 });
export const PRESSURE_ORDER = Object.freeze([
  ["right", 1, DIGITAL1.right], ["left", 1, DIGITAL1.left], ["up", 1, DIGITAL1.up], ["down", 1, DIGITAL1.down],
  ["triangle", 2, DIGITAL2.triangle], ["circle", 2, DIGITAL2.circle], ["cross", 2, DIGITAL2.cross], ["square", 2, DIGITAL2.square],
  ["l1", 2, DIGITAL2.l1], ["r1", 2, DIGITAL2.r1], ["l2", 2, DIGITAL2.l2], ["r2", 2, DIGITAL2.r2],
]);
export const NEUTRAL_PAD = Object.freeze({ digital1: 0, digital2: 0, leftX: 127, leftY: 127, rightX: 127, rightY: 127 });

function padByte(value, fallback) {
  const number = Number(value);
  if (!Number.isFinite(number)) return fallback;
  return Math.max(0, Math.min(255, Math.round(number)));
}

/**
 * Merges a partial pad state into the previous one (fields left out keep
 * their value, the same rule as the page API's setPad and the kit's
 * expandInputTrace) and resolves the pressure of every pressed
 * pressure-sensitive button: the entry's value, else the previous value
 * while the button stays pressed, else full pressure.
 * @param {Record<string, unknown> | undefined} previous
 * @param {Record<string, unknown>} entry
 */
export function mergePadState(previous, entry) {
  const base = previous ?? { ...NEUTRAL_PAD, pressure: {} };
  const digital1 = padByte(entry.digital1, /** @type {number} */ (base.digital1));
  const digital2 = padByte(entry.digital2, /** @type {number} */ (base.digital2));
  const mergedPressure = { .../** @type {Record<string, number>} */ (base.pressure ?? {}), .../** @type {Record<string, number>} */ (entry.pressure ?? {}) };
  /** @type {Record<string, number>} */
  const pressure = {};
  for (const [name, group, bit] of PRESSURE_ORDER) {
    if (((group === 1 ? digital1 : digital2) & bit) !== 0) pressure[name] = padByte(mergedPressure[name], 255);
  }
  return {
    digital1,
    digital2,
    leftX: padByte(entry.leftX, /** @type {number} */ (base.leftX)),
    leftY: padByte(entry.leftY, /** @type {number} */ (base.leftY)),
    rightX: padByte(entry.rightX, /** @type {number} */ (base.rightX)),
    rightY: padByte(entry.rightY, /** @type {number} */ (base.rightY)),
    pressure,
  };
}

/** digital1 in bits 0-7 and digital2 in bits 8-15, the host's `buttons` word. */
export function packPadButtons(state) {
  return ((Number(state.digital1) & 0xff) | ((Number(state.digital2) & 0xff) << 8)) >>> 0;
}

/** The twelve pressure bytes in .p2m2 order for a merged state (0 for released buttons). */
export function padPressureBytes(state) {
  const bytes = new Uint8Array(PRESSURE_ORDER.length);
  PRESSURE_ORDER.forEach(([name], index) => { bytes[index] = padByte(state.pressure?.[name], 0); });
  return bytes;
}

/**
 * Turns frame-indexed input trace entries (contract.js InputTraceEntry:
 * partial states that persist until the next entry for the port) into the
 * full per-entry states the host schedule takes, in frame order with
 * insertion order kept for equal frames.
 * @param {Array<Record<string, unknown>>} entries
 */
export function expandPadSchedule(entries) {
  const sorted = entries.map((entry, order) => {
    const frame = Number(entry.frame);
    if (!Number.isInteger(frame) || frame < 0) throw new Error(`inputTrace[${order}].frame must be a non-negative integer`);
    const port = Number(entry.port ?? 0);
    if (port !== 0 && port !== 1) throw new Error(`inputTrace[${order}].port must be 0 or 1`);
    return { entry, frame, port, order };
  }).sort((left, right) => left.frame - right.frame || left.order - right.order);
  /** @type {Array<Record<string, unknown> | undefined>} */
  const current = [undefined, undefined];
  return sorted.map(({ entry, frame, port }) => {
    const state = mergePadState(current[port], entry);
    current[port] = state;
    return { frame, port, ...state };
  });
}

// WebAudio::StatsIndex (pcsx2/Host/WebAudioStream.h): u32 counters at
// pcsx2_web_audio_stats_address(), in this order.
export const AUDIO_STATS = Object.freeze(["state", "sampleRate", "quantum", "callbacks", "pulledFrames", "nonzeroFrames", "underruns", "idleCallbacks", "writtenFrames"]);
export const AUDIO_STATE_NAMES = Object.freeze({ "-1": "failed", 0: "detached", 1: "starting", 2: "ready" });

/**
 * Reads the audio counters; `state` comes back signed (WebAudio::State).
 * @param {Uint32Array} heap
 * @param {number} address
 */
export function readAudioStats(heap, address) {
  const base = address >>> 2;
  /** @type {Record<string, number>} */
  const stats = {};
  AUDIO_STATS.forEach((name, index) => { stats[name] = Atomics.load(heap, base + index); });
  stats.state |= 0;
  return stats;
}

/**
 * Splits the host's trace stream into the tracerunner's two files: cpu.jsonl
 * records carry register hashes ("ee"), audio.jsonl records carry the frame
 * count and one hash ("frames").
 * @param {string} text
 */
export function splitTraceRecords(text) {
  let cpuJsonl = "";
  let audioJsonl = "";
  for (const line of text.split("\n")) {
    if (!line) continue;
    if (line.includes('"frames":')) audioJsonl += `${line}\n`;
    else cpuJsonl += `${line}\n`;
  }
  return { cpuJsonl, audioJsonl };
}
