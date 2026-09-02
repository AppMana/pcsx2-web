// Wasm/JS capabilities the port depends on, measured on the attached iPad:
// Atomics.waitAsync (event-loop-driven GS thread), synchronous
// WebAssembly.Module/Instance in a worker (JIT), tail calls, SIMD, exceptions.
//
//   node web/spikes/ipad-wasm-capabilities.mjs [pageUrlSubstring]
import { writeFile } from "node:fs/promises";
import { Connection, findPage } from "./wip.mjs";

const { device, page } = await findPage(process.argv[2] || "appmana.com");
console.log(`device ${device.deviceName} ${device.deviceOSVersion} page ${page.url}`);
const c = await new Connection(page.webSocketDebuggerUrl).open();

const workerSource = String.raw`
const hex = (s) => Uint8Array.from(s.split(" ").map((b) => parseInt(b, 16)));
// (module (func (result i32) i32.const 7) (export "f" (func 0)))
const plain = hex("00 61 73 6d 01 00 00 00 01 05 01 60 00 01 7f 03 02 01 00 07 05 01 01 66 00 00 0a 06 01 04 00 41 07 0b");
// (module (func $a (result i32) i32.const 9) (func $b (result i32) return_call $a) (export "b" (func 1)))
const tail = hex("00 61 73 6d 01 00 00 00 01 05 01 60 00 01 7f 03 03 02 00 00 07 05 01 01 62 00 01 0a 0b 02 04 00 41 09 0b 04 00 12 00 0b");
// (module (func (result v128) v128.const i32x4 1 2 3 4)) validate only
const simd = hex("00 61 73 6d 01 00 00 00 01 05 01 60 00 01 7b 03 02 01 00 0a 16 01 14 00 fd 0c 01 00 00 00 02 00 00 00 03 00 00 00 04 00 00 00 0b");
// (module (tag) (func (try_table (catch_all 0) )))  exnref-style validate
const exn = hex("00 61 73 6d 01 00 00 00 01 04 01 60 00 00 03 02 01 00 0d 03 01 00 00 0a 0a 01 08 00 1f 40 01 02 00 0b 0b");
self.onmessage = async (e) => {
  const r = {};
  try {
    const t0 = performance.now();
    const inst = new WebAssembly.Instance(new WebAssembly.Module(plain), {});
    r.syncModuleInWorker = inst.exports.f();
    r.syncCompileMs = +(performance.now() - t0).toFixed(3);
    r.tailCallValidate = WebAssembly.validate(tail);
    if (r.tailCallValidate) r.tailCallResult = new WebAssembly.Instance(new WebAssembly.Module(tail), {}).exports.b();
    r.simdValidate = WebAssembly.validate(simd);
    r.exnrefValidate = WebAssembly.validate(exn);
    r.memory64Validate = WebAssembly.validate(hex("00 61 73 6d 01 00 00 00 05 03 01 04 01"));
    r.waitAsyncType = typeof Atomics.waitAsync;
    const sab = new Int32Array(e.data.sab);
    if (Atomics.waitAsync) {
      const w = Atomics.waitAsync(sab, 0, 0, 5000);
      r.waitAsyncIsAsync = w.async;
      postMessage({ waiting: true });
      const t1 = performance.now();
      r.waitAsyncValue = await w.value;
      r.waitAsyncWakeMs = +(performance.now() - t1).toFixed(1);
    }
    r.hardwareConcurrency = navigator.hardwareConcurrency;
    for (const [name, pages] of [["shared256M", 4096], ["shared1G", 16384], ["shared2G", 32768], ["shared4G", 65536]]) {
      try { const m = new WebAssembly.Memory({ initial: 1, maximum: pages, shared: true }); r[name] = "ok"; m.grow(1); } catch (err) { r[name] = String(err); }
    }
  } catch (err) { r.error = String(err); }
  postMessage({ done: r });
};`;

const script = `(() => new Promise((resolve) => {
  const sab = new SharedArrayBuffer(4);
  const worker = new Worker(URL.createObjectURL(new Blob([${JSON.stringify(workerSource)}], { type: "text/javascript" })));
  const out = { main: { crossOriginIsolated, waitAsyncOnMain: typeof Atomics.waitAsync, ua: navigator.userAgent } };
  worker.onmessage = (e) => {
    if (e.data.waiting) { setTimeout(() => { Atomics.store(new Int32Array(sab), 0, 1); Atomics.notify(new Int32Array(sab), 0); }, 300); return; }
    out.worker = e.data.done; resolve(JSON.stringify(out));
  };
  worker.onerror = (e) => resolve(JSON.stringify({ ...out, error: e.message }));
  worker.postMessage({ sab });
}))()`;
const result = JSON.parse(await c.evaluate(script, true));
const report = { device, page: page.url, ...result };
await writeFile(new URL("./ipad-wasm-capabilities-report.json", import.meta.url), JSON.stringify(report, null, 2));
console.log(JSON.stringify(report, null, 2));
c.socket.close();
