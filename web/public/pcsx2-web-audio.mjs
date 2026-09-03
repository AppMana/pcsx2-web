// The page side of Web Audio for a module that runs in a Worker. The
// module's JS library (web/host/pcsx2_web_audio_library.js) turns every
// emscripten/webaudio.h call into a message on the port this host owns;
// here those messages become real Web Audio work on the page's
// AudioContext, including the wasm audio worklet bootstrap Emscripten's
// libwebaudio.js would otherwise run on the module's own thread: the
// module script is loaded into the AudioWorkletGlobalScope, and the
// bootstrap message carries the compiled module, the shared memory and the
// worklet thread's stack so the worklet instantiates the same program.
//
// An AnalyserNode taps the worklet node so a run can report whether the
// page actually heard something (peak level and non-zero samples).

/**
 * @param {{ audioContext: AudioContext, onState?: (state: string) => void }} options
 */
export function createAudioHost({ audioContext, onState }) {
  const channel = new MessageChannel();
  const port = channel.port1;
  /** @type {Set<number>} */
  const contextHandles = new Set();
  /** @type {Map<number, AudioWorkletNode>} */
  const nodes = new Map();
  /** @type {MessagePort | undefined} */
  let workletPort;
  /** @type {AudioWorkletNode | undefined} */
  let bootstrapNode;
  const analyser = audioContext.createAnalyser();
  analyser.fftSize = 2048;
  const window = new Float32Array(analyser.fftSize);
  const stats = { contextState: audioContext.state, sampleRate: audioContext.sampleRate, quantum: audioContext.renderQuantumSize ?? 128, workletModuleMs: 0, workletBooted: false, processors: 0, nodes: 0, peak: 0, sampledWindows: 0, nonzeroWindows: 0, nonzeroSamples: 0, forwardedCalls: 0, errors: /** @type {string[]} */ ([]) };
  let sampler;

  function sample() {
    if (!nodes.size) return;
    analyser.getFloatTimeDomainData(window);
    let peak = 0;
    let nonzero = 0;
    for (const value of window) {
      const magnitude = Math.abs(value);
      if (magnitude > peak) peak = magnitude;
      if (value !== 0) nonzero += 1;
    }
    stats.sampledWindows += 1;
    if (nonzero) stats.nonzeroWindows += 1;
    stats.nonzeroSamples += nonzero;
    if (peak > stats.peak) stats.peak = peak;
  }

  function sendState(handle) {
    port.postMessage({ op: "context-state", handle, state: audioContext.state, sampleRate: audioContext.sampleRate, quantum: stats.quantum });
  }

  const onStateChange = () => {
    stats.contextState = audioContext.state;
    for (const handle of contextHandles) sendState(handle);
    onState?.(audioContext.state);
  };
  audioContext.addEventListener("statechange", onStateChange);

  // libwebaudio.js's _boot message: the worklet's copy of the runtime starts
  // from it (startWasmWorker in the module script). Chrome still has no
  // AudioWorklet.port, so the message travels as processorOptions of the
  // 'em-bootstrap' node the module script registers.
  async function bootWorklet(request) {
    const startedAt = performance.now();
    await audioContext.audioWorklet.addModule(request.scriptUrl);
    stats.workletModuleMs = performance.now() - startedAt;
    const boot = { _boot: 1, wwID: request.wwID, wasm: request.wasm, wasmMemory: request.wasmMemory, stackLowestAddress: request.stackLowestAddress, stackSize: request.stackSize, pthreadPtr: request.pthreadPtr };
    const worklet = /** @type {AudioWorklet & { port?: MessagePort }} */ (audioContext.audioWorklet);
    if (worklet.port) {
      workletPort = worklet.port;
      workletPort.postMessage(boot);
    } else {
      bootstrapNode = new AudioWorkletNode(audioContext, "em-bootstrap", { processorOptions: boot });
      workletPort = bootstrapNode.port;
    }
    // Everything the worklet posts back is a wasm call for the module's thread.
    workletPort.onmessage = (event) => {
      stats.forwardedCalls += 1;
      port.postMessage(event.data);
    };
    stats.workletBooted = true;
  }

  port.onmessage = async ({ data }) => {
    try {
      switch (data.op) {
        case "context":
          contextHandles.add(data.handle);
          sendState(data.handle);
          break;
        case "resume":
          await audioContext.resume();
          stats.contextState = audioContext.state;
          if (data.id !== undefined) port.postMessage({ op: "resumed", id: data.id, handle: data.handle, state: audioContext.state });
          break;
        case "worklet":
          try {
            await bootWorklet(data);
            port.postMessage({ op: "worklet-ready", id: data.id, ok: true });
          } catch (error) {
            stats.errors.push(`worklet: ${error instanceof Error ? error.message : String(error)}`);
            port.postMessage({ op: "worklet-ready", id: data.id, ok: false, error: String(error) });
          }
          break;
        case "processor":
          if (!workletPort) throw new Error("processor requested before the worklet booted");
          stats.processors += 1;
          workletPort.postMessage({ _wpn: data.name, contextHandle: data.contextHandle, callback: data.callback, userData: data.userData });
          break;
        case "node": {
          const node = new AudioWorkletNode(audioContext, data.name, {
            ...(data.options ?? {}),
            processorOptions: { callback: data.callback, userData: data.userData, samplesPerChannel: data.samplesPerChannel },
          });
          node.addEventListener("processorerror", (event) => { stats.errors.push(`processor: ${/** @type {ErrorEvent} */ (event).message ?? "error"}`); });
          nodes.set(data.handle, node);
          node.connect(analyser);
          stats.nodes = nodes.size;
          sampler ??= setInterval(sample, 50);
          break;
        }
        case "connect": {
          const source = nodes.get(data.source);
          if (!source) throw new Error(`connect: unknown source ${data.source}`);
          const destination = contextHandles.has(data.destination) ? audioContext.destination : nodes.get(data.destination);
          if (!destination) throw new Error(`connect: unknown destination ${data.destination}`);
          source.connect(destination, data.outputIndex, data.inputIndex);
          break;
        }
        case "destroy-node":
          nodes.get(data.handle)?.disconnect();
          nodes.delete(data.handle);
          stats.nodes = nodes.size;
          break;
        case "destroy-context":
          contextHandles.delete(data.handle);
          break;
        default:
          throw new Error(`unknown audio message ${String(data.op)}`);
      }
    } catch (error) {
      stats.errors.push(`${data?.op}: ${error instanceof Error ? error.message : String(error)}`);
    }
  };

  return {
    /** The port to transfer to the module's worker. */
    port: channel.port2,
    stats: () => ({ ...stats, contextState: audioContext.state, errors: [...stats.errors] }),
    close() {
      clearInterval(sampler);
      sampler = undefined;
      for (const node of nodes.values()) node.disconnect();
      nodes.clear();
      bootstrapNode?.disconnect();
      audioContext.removeEventListener("statechange", onStateChange);
      port.close();
    },
  };
}
