// WebKit Inspector Protocol client for one USB-attached iPad behind
// ios_webkit_debug_proxy (discovery on 127.0.0.1:9221). Shared by the spikes
// until the harness package replaces it.
export const delay = (ms) => new Promise((r) => setTimeout(r, ms));

export async function findPage(match, discovery = process.env.WIP_DISCOVERY_URL || "http://127.0.0.1:9221/json") {
  const devices = await (await fetch(discovery)).json();
  if (devices.length !== 1) throw new Error(`expected one device, found ${devices.length}`);
  const pages = await (await fetch(`http://${devices[0].url}/json`)).json();
  const page = pages.find((p) => p.url.includes(match) && p.webSocketDebuggerUrl);
  if (!page) throw new Error(`no page matching ${match}: ${pages.map((p) => p.url).join(", ")}`);
  return { device: devices[0], page };
}

export class Connection {
  constructor(url) { this.socket = new WebSocket(url); this.targetId = undefined; this.inner = 0; this.outer = 0; this.pending = new Map(); }
  async open() {
    this.socket.addEventListener("message", (e) => this.onMessage(e));
    this.socket.addEventListener("close", (e) => { console.log(`socket closed code=${e.code} reason=${e.reason}`); for (const p of this.pending.values()) p.reject(new Error("socket closed")); this.pending.clear(); });
    await new Promise((res, rej) => { this.socket.addEventListener("open", res, { once: true }); this.socket.addEventListener("error", rej, { once: true }); });
    const deadline = Date.now() + 15000;
    while (!this.targetId && Date.now() < deadline) await delay(25);
    if (!this.targetId) throw new Error("no page target announced");
    return this;
  }
  onMessage(event) {
    const m = JSON.parse(event.data);
    if (m.method === "Target.targetCreated" && !this.targetId && m.params.targetInfo.type === "page" && !m.params.targetInfo.isProvisional) this.targetId = m.params.targetInfo.targetId;
    if (m.method === "Target.didCommitProvisionalTarget" && m.params.oldTargetId === this.targetId) { console.log("target replaced"); this.targetId = m.params.newTargetId; }
    if (m.method === "Target.targetDestroyed") console.log(`target destroyed ${m.params.targetId}`);
    if (m.method !== "Target.dispatchMessageFromTarget") return;
    const inner = JSON.parse(m.params.message);
    const p = this.pending.get(inner.id); if (!p) return; this.pending.delete(inner.id);
    inner.error ? p.reject(new Error(inner.error.message)) : p.resolve(inner.result);
  }
  command(method, params = {}) {
    const id = ++this.inner;
    this.socket.send(JSON.stringify({ id: ++this.outer, method: "Target.sendMessageToTarget", params: { targetId: this.targetId, message: JSON.stringify({ id, method, params }) } }));
    return new Promise((resolve, reject) => {
      const timer = setTimeout(() => { this.pending.delete(id); reject(new Error(`${method} timed out`)); }, 30000);
      this.pending.set(id, { resolve: (v) => { clearTimeout(timer); resolve(v); }, reject: (e) => { clearTimeout(timer); reject(e); } });
    });
  }
  async evaluate(expression, awaitPromise = false) {
    const ev = await this.command("Runtime.evaluate", { expression, returnByValue: !awaitPromise, doNotPauseOnExceptionsAndMuteConsole: true });
    if (ev.wasThrown) throw new Error(ev.result.description || "evaluation failed");
    let result = ev.result;
    if (awaitPromise && result.objectId) {
      const aw = await this.command("Runtime.awaitPromise", { promiseObjectId: result.objectId, returnByValue: true });
      if (aw.wasThrown) throw new Error(aw.result.description || "promise rejected");
      result = aw.result;
    }
    return result.value;
  }
}

