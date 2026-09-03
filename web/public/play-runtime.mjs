// Interactive page over the runtime page API: picks a stored BIOS and a
// program, runs it until stop with audio through the page's AudioContext
// (created on the start click, the user gesture browsers require), and
// drives the pad from the keyboard and the touch buttons through
// __pcsx2Runtime.setPad. ?boot=<url> names a program outside the fixture
// list; ?trace=<url> replays a recorded input trace through the schedule.
// Inputs are recorded either way and exported by the trace button.
import "./runtime-acceptance.mjs";
import { ensureAudioContext } from "./runtime-acceptance.mjs";
import { DIGITAL1, DIGITAL2 } from "./pcsx2-report.mjs";

const FIXTURES = ["hello_tty", "gs_sprite", "gs_blend", "vu1_cube", "pad_echo"].map((name) => `tests/fixtures/${name}/${name}.elf`);
const BIOS_DIR = "pcsx2/bios";

const keyControls = new Map([
  ["ArrowUp", "up"], ["ArrowRight", "right"], ["ArrowDown", "down"], ["ArrowLeft", "left"],
  ["Enter", "start"], ["ShiftLeft", "select"], ["ShiftRight", "select"],
  ["KeyZ", "cross"], ["KeyX", "circle"], ["KeyA", "square"], ["KeyS", "triangle"],
  ["KeyQ", "l1"], ["KeyW", "r1"], ["KeyE", "l2"], ["KeyR", "r2"], ["KeyC", "l3"], ["KeyV", "r3"],
  ["KeyI", "leftUp"], ["KeyK", "leftDown"], ["KeyJ", "leftLeft"], ["KeyL", "leftRight"],
  ["KeyT", "rightUp"], ["KeyG", "rightDown"], ["KeyF", "rightLeft"], ["KeyH", "rightRight"],
]);
const STICKS = { leftUp: ["leftY", 0], leftDown: ["leftY", 255], leftLeft: ["leftX", 0], leftRight: ["leftX", 255], rightUp: ["rightY", 0], rightDown: ["rightY", 255], rightLeft: ["rightX", 0], rightRight: ["rightX", 255] };

const statusElement = document.querySelector("#status");
const detailElement = document.querySelector("#detail");
const biosSelect = document.querySelector("#bios");
const targetSelect = document.querySelector("#target");
const webgpuCheckbox = document.querySelector("#webgpu");
const params = new URLSearchParams(location.search);
const keys = new Set();
const touches = new Map();
let running = false;

function runtime() {
  return window.__pcsx2Runtime;
}

function controlState() {
  const controls = new Set([...keys].map((code) => keyControls.get(code)).filter(Boolean));
  touches.forEach((control) => controls.add(control));
  const state = { digital1: 0, digital2: 0, leftX: 127, leftY: 127, rightX: 127, rightY: 127 };
  for (const control of controls) {
    if (control in DIGITAL1) state.digital1 |= DIGITAL1[control];
    else if (control in DIGITAL2) state.digital2 |= DIGITAL2[control];
    else if (control in STICKS) state[STICKS[control][0]] = STICKS[control][1];
  }
  return state;
}

function sendPad() {
  runtime()?.setPad(controlState());
  document.querySelectorAll("[data-control]").forEach((button) => {
    const pressed = [...touches.values()].includes(button.dataset.control) || [...keys].some((code) => keyControls.get(code) === button.dataset.control);
    button.classList.toggle("pressed", pressed);
  });
}

async function listBios() {
  const names = [];
  try {
    let directory = await navigator.storage.getDirectory();
    for (const part of BIOS_DIR.split("/")) directory = await directory.getDirectoryHandle(part);
    for await (const [name, handle] of directory.entries()) if (handle.kind === "file" && /\.bin$/i.test(name)) names.push(name);
  } catch {}
  names.sort();
  biosSelect.replaceChildren(...names.map((name) => Object.assign(document.createElement("option"), { value: `${BIOS_DIR}/${name}`, textContent: name })));
  if (!names.length) biosSelect.append(Object.assign(document.createElement("option"), { value: "", textContent: "none imported" }));
}

function listTargets() {
  const targets = [...FIXTURES];
  const boot = params.get("boot");
  if (boot && !targets.includes(boot)) targets.unshift(boot);
  targetSelect.replaceChildren(...targets.map((target) => Object.assign(document.createElement("option"), { value: target, textContent: target.split("/").pop() })));
  if (boot) targetSelect.value = boot;
  webgpuCheckbox.checked = Boolean(navigator.gpu);
  webgpuCheckbox.disabled = !navigator.gpu;
}

async function loadTrace() {
  const url = params.get("trace");
  if (!url) return undefined;
  const response = await fetch(url);
  if (!response.ok) throw new Error(`input trace fetch returned ${response.status}`);
  const trace = await response.json();
  return Array.isArray(trace) ? trace : trace.entries;
}

async function start() {
  if (running || !runtime()) return;
  running = true;
  try {
    // The AudioContext is created here, inside the click, before anything awaits.
    const audioContext = await ensureAudioContext();
    detailElement.textContent = `audio ${audioContext.state} at ${audioContext.sampleRate} Hz; loading`;
    const inputTrace = await loadTrace();
    const report = await runtime().run(targetSelect.value, {
      frames: 0,
      timeoutMs: 0,
      audio: true,
      bios: biosSelect.value || undefined,
      render: webgpuCheckbox.checked,
      renderer: webgpuCheckbox.checked ? "webgpu" : undefined,
      settings: { "EmuCore/GS/FrameLimitEnable": "true" },
      inputTrace,
      pad: controlState(),
    });
    const audio = report.emu?.audio ?? {};
    detailElement.textContent = `${report.detail}; audio ${audio.detail ?? "off"}, worklet pulled ${audio.worklet?.pulledFrames ?? 0} frames (${audio.worklet?.nonzeroFrames ?? 0} non-silent, ${audio.worklet?.underruns ?? 0} underruns); pad applied ${report.emu?.inputTrace?.applied ?? 0} times`;
  } catch (error) {
    statusElement.textContent = "failed";
    detailElement.textContent = error instanceof Error ? error.message : String(error);
  } finally {
    running = false;
  }
}

function stop() {
  runtime()?.stop();
}

for (const type of ["keydown", "keyup"]) {
  addEventListener(type, (event) => {
    if (!keyControls.has(event.code) || event.target instanceof HTMLSelectElement) return;
    event.preventDefault();
    if (type === "keydown") keys.add(event.code);
    else keys.delete(event.code);
    sendPad();
  });
}
addEventListener("blur", () => { keys.clear(); touches.clear(); sendPad(); });

document.querySelectorAll("[data-control]").forEach((button) => {
  button.addEventListener("pointerdown", (event) => {
    event.preventDefault();
    button.setPointerCapture(event.pointerId);
    touches.set(event.pointerId, button.dataset.control);
    sendPad();
  });
  for (const type of ["pointerup", "pointercancel", "lostpointercapture"]) {
    button.addEventListener(type, (event) => { touches.delete(event.pointerId); sendPad(); });
  }
});

document.querySelector("#start").addEventListener("click", () => { void start(); });
document.querySelector("#stop").addEventListener("click", stop);
document.querySelector("#export-trace").addEventListener("click", async () => {
  const trace = await runtime()?.exportInputTrace();
  if (!trace) return;
  const json = JSON.stringify({ schema: 1, target: targetSelect.value, entries: trace.entries, flipCounter: trace.flipCounter });
  try { await navigator.clipboard.writeText(json); } catch {}
  detailElement.textContent = `input trace: ${trace.entries.length} entries through frame ${trace.flipCounter}, ${trace.applied} applications (copied to clipboard)`;
});

window.__pcsx2Playable = { start, stop, setPad: (state) => runtime()?.setPad(state), exportInputTrace: () => runtime()?.exportInputTrace(), controlState };

listTargets();
void listBios();
