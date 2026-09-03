const worker = new Worker("./unit-worker.mjs", { type: "module" });
const result = await new Promise((resolve, reject) => {
  const timer = setTimeout(() => reject(new Error("PCSX2 wasm units timed out")), 120_000);
  worker.addEventListener("message", (event) => {
    if (event.data?.type !== "unit-result") return;
    clearTimeout(timer);
    resolve(event.data);
  });
  worker.addEventListener("error", (event) => {
    clearTimeout(timer);
    reject(new Error(event.message || "PCSX2 wasm unit worker failed"));
  });
  worker.postMessage({ type: "run-units" });
}).catch((error) => ({ type: "unit-result", ok: false, error: error instanceof Error ? `${error.name}: ${error.message}` : String(error) }));

globalThis.__pcsx2UnitResult = {
  ...result,
  userAgent: navigator.userAgent,
  crossOriginIsolated,
};
document.querySelector("#status").textContent = JSON.stringify(globalThis.__pcsx2UnitResult, null, 2);
worker.terminate();
