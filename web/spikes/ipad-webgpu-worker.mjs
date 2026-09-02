// Spike 1+2 from the plan: does Mobile Safari expose WebGPU inside a Web Worker
// on a transferred OffscreenCanvas, and is a frame rendered by that worker
// presented while the worker then blocks in Atomics.wait?
//
//   node web/spikes/ipad-webgpu-worker.mjs [pageUrlSubstring]
import { writeFile } from "node:fs/promises";
import { Connection, delay, findPage } from "./wip.mjs";

const match = process.argv[2] || "appmana.com";
const { device, page } = await findPage(match);
const devices = [device];
console.log(`device ${device.deviceName} ${device.deviceOSVersion} page ${page.url}`);

const c = await new Connection(page.webSocketDebuggerUrl).open();

const workerSource = String.raw`
self.onmessage = async (e) => {
  const r = { gpuInWorker: !!self.navigator.gpu, offscreenCanvas: typeof OffscreenCanvas !== "undefined" };
  try {
    const adapter = await navigator.gpu.requestAdapter();
    r.adapter = !!adapter;
    if (adapter) { r.features = [...adapter.features]; r.info = adapter.info ? { vendor: adapter.info.vendor, architecture: adapter.info.architecture, device: adapter.info.device } : null; }
    const device = await adapter.requestDevice();
    const canvas = e.data.canvas;
    const ctx = canvas.getContext("webgpu");
    r.context = !!ctx;
    ctx.configure({ device, format: navigator.gpu.getPreferredCanvasFormat(), alphaMode: "opaque" });
    const enc = device.createCommandEncoder();
    const pass = enc.beginRenderPass({ colorAttachments: [{ view: ctx.getCurrentTexture().createView(), loadOp: "clear", clearValue: { r: 0, g: 1, b: 0, a: 1 }, storeOp: "store" }] });
    pass.end();
    device.queue.submit([enc.finish()]);
    r.rendered = true;
    const sab = new Int32Array(new SharedArrayBuffer(4));
    const t0 = performance.now();
    r.waitResult = Atomics.wait(sab, 0, 0, 3000);
    r.blockedMs = Math.round(performance.now() - t0);
  } catch (err) { r.error = String(err); }
  postMessage(r);
};`;

const setup = `(() => {
  const old = document.getElementById("spike-canvas"); if (old) old.remove();
  const canvas = document.createElement("canvas");
  canvas.id = "spike-canvas"; canvas.width = 64; canvas.height = 64;
  canvas.style.cssText = "position:fixed;top:0;left:0;width:64px;height:64px;z-index:2147483647";
  document.body.appendChild(canvas);
  const off = canvas.transferControlToOffscreen();
  const worker = new Worker(URL.createObjectURL(new Blob([${JSON.stringify(workerSource)}], { type: "text/javascript" })));
  window.__spike = { crossOriginIsolated, gpuOnMain: !!navigator.gpu, sab: typeof SharedArrayBuffer !== "undefined", ua: navigator.userAgent, hw: navigator.hardwareConcurrency, canvas, worker: null };
  window.__spike.done = new Promise((res) => { worker.onmessage = (e) => { window.__spike.worker = e.data; res(e.data); }; worker.onerror = (e) => { window.__spike.worker = { error: e.message }; res(window.__spike.worker); }; });
  worker.postMessage({ canvas: off }, [off]);
  return "started";
})()`;
console.log("setup:", await c.evaluate(setup));

// Sample the placeholder canvas from the page while the worker is blocked.
const samplePixel = `(() => {
  const src = document.getElementById("spike-canvas");
  const probe = document.createElement("canvas"); probe.width = 4; probe.height = 4;
  const g = probe.getContext("2d");
  try { g.drawImage(src, 0, 0, 4, 4); const d = g.getImageData(1, 1, 1, 1).data; return { rgba: [...d], workerDone: !!window.__spike.worker }; }
  catch (e) { return { error: String(e), workerDone: !!window.__spike.worker }; }
})()`;
const samples = [];
for (let i = 0; i < 5; i++) { await delay(500); samples.push({ tMs: (i + 1) * 500, ...(await c.evaluate(samplePixel)) }); }
let snapshotPixel = null;
try {
  const snap = await c.command("Page.snapshotRect", { x: 0, y: 0, width: 64, height: 64, coordinateSystem: "Viewport" });
  const b64 = snap.dataURL?.match(/^data:image\/png;base64,(.+)$/)?.[1];
  if (b64) { await writeFile(new URL("./ipad-webgpu-worker-during-block.png", import.meta.url), Buffer.from(b64, "base64")); snapshotPixel = "saved ipad-webgpu-worker-during-block.png"; }
} catch (e) { snapshotPixel = `snapshotRect failed: ${e.message}`; }

const worker = await c.evaluate("window.__spike.done", true);
const main = await c.evaluate("({crossOriginIsolated: window.__spike.crossOriginIsolated, gpuOnMain: window.__spike.gpuOnMain, sab: window.__spike.sab, ua: window.__spike.ua, hw: window.__spike.hw})");
const after = await c.evaluate(samplePixel);
const report = { device: devices[0], page: page.url, main, worker, samplesDuringBlock: samples, sampleAfter: after, snapshotPixel };
await writeFile(new URL("./ipad-webgpu-worker-report.json", import.meta.url), JSON.stringify(report, null, 2));
console.log(JSON.stringify(report, null, 2));
c.socket.close();
