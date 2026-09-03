// Web Audio glue for a module that lives in a Worker while the AudioContext
// lives on the page. Emscripten's libwebaudio.js assumes the thread calling
// emscripten/webaudio.h owns the AudioContext; here every call becomes a
// message to the page (web/public/pcsx2-web-audio.mjs), which performs the
// real Web Audio work and bootstraps the wasm audio worklet from the same
// module and shared memory. Context and node handles are proxy records in
// emAudio so the C++ side uses the standard API unchanged.
//
// The page hands its MessagePort to the module as Module.pcsx2AudioPort and
// the module script URL as Module.pcsx2AudioScriptUrl before
// pcsx2_web_audio_attach() runs.
addToLibrary({
  $pcsx2Audio: {
    port: null,
    nextId: 1,
    pending: {},
  },

  $pcsx2AudioInit__deps: ['$pcsx2Audio', '$pcsx2AudioReceive'],
  $pcsx2AudioInit: () => {
    if (pcsx2Audio.port) return true;
    var port = Module['pcsx2AudioPort'];
    if (!port) return false;
    port.onmessage = (event) => pcsx2AudioReceive(event.data);
    pcsx2Audio.port = port;
    return true;
  },

  $pcsx2AudioSend__deps: ['$pcsx2AudioInit'],
  $pcsx2AudioSend: (message) => {
    if (!pcsx2AudioInit()) {
      err('pcsx2 audio: no page port, dropping ' + message.op);
      return false;
    }
    pcsx2Audio.port.postMessage(message);
    return true;
  },

  $pcsx2AudioReceive__deps: ['$pcsx2Audio', '$emAudio', '$getWasmTableEntry'],
  $pcsx2AudioReceive: (d) => {
    // '_wsc': a wasm call the audio worklet posted to this thread (processor
    // registration completions and emscripten_audio_worklet_post_function_*),
    // forwarded by the page unchanged.
    if (d['_wsc']) {
      getWasmTableEntry(d['_wsc'])(...d.args);
      return;
    }
    var ctx;
    switch (d.op) {
      case 'context-state':
        ctx = emAudio[d.handle];
        if (ctx) {
          ctx.state = d.state;
          ctx.sampleRate = d.sampleRate;
          ctx.renderQuantumSize = d.quantum;
        }
        break;
      case 'worklet-ready': {
        var pending = pcsx2Audio.pending[d.id];
        delete pcsx2Audio.pending[d.id];
        if (!d.ok) err('pcsx2 audio: worklet bootstrap failed: ' + d.error);
        if (pending) {{{ makeDynCall('viip', 'pending.callback') }}}(pending.contextHandle, d.ok ? 1 : 0, pending.userData);
        break;
      }
      case 'resumed': {
        var resume = pcsx2Audio.pending[d.id];
        delete pcsx2Audio.pending[d.id];
        ctx = emAudio[d.handle];
        if (ctx) ctx.state = d.state;
        if (resume) {{{ makeDynCall('viip', 'resume.callback') }}}(d.handle, d.state == 'running' ? 1 : 0, resume.userData);
        break;
      }
      default:
        break;
    }
  },

  emscripten_create_audio_context__deps: ['$emscriptenRegisterAudioObject', '$pcsx2AudioSend'],
  emscripten_create_audio_context: (options) => {
    var sampleRate = options ? {{{ makeGetValue('options', C_STRUCTS.EmscriptenWebAudioCreateAttributes.sampleRate, 'u32') }}} : 0;
    var latencyHint = options ? UTF8ToString({{{ makeGetValue('options', C_STRUCTS.EmscriptenWebAudioCreateAttributes.latencyHint, '*') }}}) : '';
    // The page already holds the AudioContext; this handle names it from wasm.
    var proxy = { pcsx2: 'context', state: 'suspended', sampleRate: sampleRate, renderQuantumSize: 128 };
    var handle = emscriptenRegisterAudioObject(proxy);
    pcsx2AudioSend({ op: 'context', handle: handle, sampleRate: sampleRate, latencyHint: latencyHint });
    return handle;
  },

  emscripten_resume_audio_context_async__deps: ['$pcsx2Audio', '$pcsx2AudioSend'],
  emscripten_resume_audio_context_async: (contextHandle, callback, userData) => {
    var id = pcsx2Audio.nextId++;
    pcsx2Audio.pending[id] = { callback: callback, userData: userData };
    pcsx2AudioSend({ op: 'resume', handle: contextHandle, id: id });
  },

  emscripten_resume_audio_context_sync__deps: ['$pcsx2AudioSend'],
  emscripten_resume_audio_context_sync: (contextHandle) => {
    pcsx2AudioSend({ op: 'resume', handle: contextHandle });
  },

  emscripten_audio_context_state__deps: ['$emAudio'],
  emscripten_audio_context_state: (contextHandle) => {
    return ['suspended', 'running', 'closed', 'interrupted'].indexOf(emAudio[contextHandle].state);
  },

  emscripten_destroy_audio_context__deps: ['$emAudio', '$pcsx2AudioSend'],
  emscripten_destroy_audio_context: (contextHandle) => {
    pcsx2AudioSend({ op: 'destroy-context', handle: contextHandle });
    delete emAudio[contextHandle];
  },

  emscripten_destroy_web_audio_node__deps: ['$emAudio', '$pcsx2AudioSend'],
  emscripten_destroy_web_audio_node: (objectHandle) => {
    pcsx2AudioSend({ op: 'destroy-node', handle: objectHandle });
    delete emAudio[objectHandle];
  },

  // The wasm worker id, pthread block and stack were prepared by
  // emscripten_start_wasm_audio_worklet_thread_async() in C; the page loads
  // this module's script into the AudioWorkletGlobalScope and posts the
  // same bootstrap message libwebaudio.js would.
  _emscripten_create_audio_worklet__deps: ['$pcsx2Audio', '$pcsx2AudioSend'],
  _emscripten_create_audio_worklet: (wwID, contextHandle, stackLowestAddress, stackSize, pthreadPtr, callback, userData) => {
    var id = pcsx2Audio.nextId++;
    pcsx2Audio.pending[id] = { callback: callback, userData: userData, contextHandle: contextHandle };
    var sent = pcsx2AudioSend({
      op: 'worklet',
      id: id,
      handle: contextHandle,
      wwID: wwID,
      wasm: wasmModule,
      wasmMemory: wasmMemory,
      stackLowestAddress: stackLowestAddress,
      stackSize: stackSize,
      pthreadPtr: pthreadPtr,
      scriptUrl: Module['pcsx2AudioScriptUrl'],
    });
    if (!sent) {
      delete pcsx2Audio.pending[id];
      {{{ makeDynCall('viip', 'callback') }}}(contextHandle, 0, userData);
    }
  },

  emscripten_create_wasm_audio_worklet_processor_async__deps: ['$pcsx2AudioSend'],
  emscripten_create_wasm_audio_worklet_processor_async: (contextHandle, options, callback, userData) => {
    var name = UTF8ToString({{{ makeGetValue('options', C_STRUCTS.WebAudioWorkletProcessorCreateOptions.name, '*') }}});
    // The worklet answers with a '_wsc' call of `callback`, which the page forwards here.
    pcsx2AudioSend({ op: 'processor', handle: contextHandle, name: name, contextHandle: contextHandle, callback: callback, userData: userData });
  },

  emscripten_create_wasm_audio_worklet_node__deps: ['$emAudio', '$emscriptenRegisterAudioObject', '$pcsx2AudioSend'],
  emscripten_create_wasm_audio_worklet_node: (contextHandle, name, options, callback, userData) => {
    function readChannelCountArray(heapIndex, numOutputs) {
      if (!heapIndex) return undefined;
      heapIndex = {{{ getHeapOffset('heapIndex', 'i32') }}};
      var channelCounts = [];
      while (numOutputs--) channelCounts.push(HEAPU32[heapIndex++]);
      return channelCounts;
    }
    var optionsOutputs = options ? {{{ makeGetValue('options', C_STRUCTS.EmscriptenAudioWorkletNodeCreateOptions.numberOfOutputs, 'i32') }}} : 0;
    var opts = options ? {
      numberOfInputs: {{{ makeGetValue('options', C_STRUCTS.EmscriptenAudioWorkletNodeCreateOptions.numberOfInputs, 'i32') }}},
      numberOfOutputs: optionsOutputs,
      outputChannelCount: readChannelCountArray({{{ makeGetValue('options', C_STRUCTS.EmscriptenAudioWorkletNodeCreateOptions.outputChannelCounts, 'i32*') }}}, optionsOutputs),
      channelCount: {{{ makeGetValue('options', C_STRUCTS.EmscriptenAudioWorkletNodeCreateOptions.channelCount, 'u32') }}} || undefined,
      channelCountMode: [/*'max'*/,'clamped-max','explicit'][{{{ makeGetValue('options', C_STRUCTS.EmscriptenAudioWorkletNodeCreateOptions.channelCountMode, 'i32') }}}],
      channelInterpretation: [/*'speakers'*/,'discrete'][{{{ makeGetValue('options', C_STRUCTS.EmscriptenAudioWorkletNodeCreateOptions.channelInterpretation, 'i32') }}}],
    } : undefined;
    var ctx = emAudio[contextHandle];
    var handle = emscriptenRegisterAudioObject({ pcsx2: 'node', name: UTF8ToString(name) });
    pcsx2AudioSend({
      op: 'node',
      handle: handle,
      contextHandle: contextHandle,
      name: UTF8ToString(name),
      options: opts,
      callback: callback,
      userData: userData,
      samplesPerChannel: (ctx && ctx.renderQuantumSize) || 128,
    });
    return handle;
  },

  emscripten_audio_context_quantum_size__deps: ['$emAudio'],
  emscripten_audio_context_quantum_size: (contextHandle) => {
    return emAudio[contextHandle].renderQuantumSize || 128;
  },

  emscripten_audio_context_sample_rate__deps: ['$emAudio'],
  emscripten_audio_context_sample_rate: (contextHandle) => {
    return emAudio[contextHandle].sampleRate;
  },

  emscripten_audio_node_connect__deps: ['$pcsx2AudioSend'],
  emscripten_audio_node_connect: (source, destination, outputIndex, inputIndex) => {
    pcsx2AudioSend({ op: 'connect', source: source, destination: destination, outputIndex: outputIndex, inputIndex: inputIndex });
  },
});
