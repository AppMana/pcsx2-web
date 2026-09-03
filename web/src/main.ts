import "./style.css";
import { probeWebGpuCapabilities } from "virtual:web-emulator-kit/webgpu-probe";
import type { WebGpuCapabilities } from "@appmana-public/web-emulator-harness/playwright";
import { detectWasmFeatures, sharedMemoryGrows, type WasmFeatures } from "./wasm-features";

export type CapabilityReport = {
  schema: 1;
  capturedAt: string;
  userAgent: string;
  hardwareConcurrency: number;
  crossOriginIsolated: boolean;
  sharedArrayBuffer: boolean;
  sharedMemoryGrows: boolean;
  wasm: WasmFeatures;
  atomicsWaitAsync: boolean;
  dedicatedWorker: boolean;
  opfs: boolean;
  offscreenCanvas: boolean;
  webGpu: WebGpuCapabilities;
};

declare global {
  interface Window {
    __pcsx2Capabilities?: Promise<CapabilityReport>;
  }
}

const app = document.querySelector<HTMLDivElement>("#app")!;

function escapeHtml(value: string): string {
  return value.replace(/[&<>'"]/g, (character) => ({ "&": "&amp;", "<": "&lt;", ">": "&gt;", "'": "&#39;", '"': "&quot;" })[character] ?? character);
}

function row(label: string, ok: boolean, detail = ""): string {
  return `<div class="check"><span>${escapeHtml(label)}</span><strong class="${ok ? "passed" : "failed"}">${ok ? "yes" : "no"}</strong><small>${escapeHtml(detail)}</small></div>`;
}

async function collect(): Promise<CapabilityReport> {
  const webGpu = await probeWebGpuCapabilities();
  return {
    schema: 1,
    capturedAt: new Date().toISOString(),
    userAgent: navigator.userAgent,
    hardwareConcurrency: navigator.hardwareConcurrency,
    crossOriginIsolated: window.crossOriginIsolated,
    sharedArrayBuffer: typeof SharedArrayBuffer === "function",
    sharedMemoryGrows: sharedMemoryGrows(),
    wasm: detectWasmFeatures(),
    atomicsWaitAsync: typeof Atomics.waitAsync === "function",
    dedicatedWorker: typeof Worker === "function",
    opfs: typeof navigator.storage?.getDirectory === "function",
    offscreenCanvas: typeof OffscreenCanvas === "function",
    webGpu,
  };
}

function render(report: CapabilityReport): void {
  const adapter = report.webGpu.adapter;
  const adapterDetail = adapter
    ? [adapter.vendor, adapter.architecture, adapter.device, adapter.description].filter(Boolean).join(" · ") + (adapter.isFallbackAdapter ? " (fallback)" : "")
    : "no adapter";
  app.innerHTML = `
    <header>
      <p class="eyebrow">PCSX2 · wasm · WebGPU · Mobile Safari</p>
      <h1>PCSX2 in the browser</h1>
      <p class="lede">Capability report for this browser. The runtime, storage and unit test pages are linked below.</p>
      <nav>
        <a href="./runtime.html">runtime.html</a>
        <a href="./play.html">play.html</a>
        <a href="./storage.html">storage.html</a>
        <a href="./units.html">units.html</a>
      </nav>
    </header>
    <main>
      <section>
        ${row("crossOriginIsolated", report.crossOriginIsolated, "COOP/COEP headers or the coi service worker")}
        ${row("SharedArrayBuffer", report.sharedArrayBuffer)}
        ${row("shared wasm memory grows", report.sharedMemoryGrows, "initial 64 KiB, maximum 2 GiB")}
        ${row("wasm SIMD", report.wasm.simd)}
        ${row("wasm threads", report.wasm.threads)}
        ${row("wasm tail calls", report.wasm.tailCall)}
        ${row("wasm exceptions", report.wasm.exceptions)}
        ${row("Atomics.waitAsync", report.atomicsWaitAsync)}
        ${row("dedicated workers", report.dedicatedWorker)}
        ${row("origin-private file system", report.opfs)}
        ${row("OffscreenCanvas", report.offscreenCanvas)}
        ${row("WebGPU adapter", report.webGpu.webGpu, adapterDetail)}
        ${adapter ? `<p class="features">features: ${escapeHtml(adapter.features.join(", ") || "none")}</p>` : ""}
      </section>
      <details open><summary>Report JSON</summary><pre id="evidence">${escapeHtml(JSON.stringify(report, null, 2))}</pre></details>
    </main>
    <footer>${escapeHtml(report.userAgent)} · ${report.hardwareConcurrency} logical cores</footer>
  `;
}

app.innerHTML = "<p class=\"lede\">Collecting capabilities…</p>";
window.__pcsx2Capabilities = collect();
window.__pcsx2Capabilities.then(render, (error: unknown) => {
  app.innerHTML = `<p class="lede failed">${escapeHtml(error instanceof Error ? `${error.name}: ${error.message}` : String(error))}</p>`;
});
