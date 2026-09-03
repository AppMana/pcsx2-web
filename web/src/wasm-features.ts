// WebAssembly feature detection by validating the smallest module that uses
// each feature (the wasm-feature-detect approach, inlined so the page has no
// runtime dependency for it).
const MODULES: Record<WasmFeature, number[]> = {
  simd: [0, 97, 115, 109, 1, 0, 0, 0, 1, 5, 1, 96, 0, 1, 123, 3, 2, 1, 0, 10, 10, 1, 8, 0, 65, 0, 253, 15, 253, 98, 11],
  threads: [0, 97, 115, 109, 1, 0, 0, 0, 1, 4, 1, 96, 0, 0, 3, 2, 1, 0, 5, 4, 1, 3, 1, 1, 10, 11, 1, 9, 0, 65, 0, 254, 16, 2, 0, 26, 11],
  tailCall: [0, 97, 115, 109, 1, 0, 0, 0, 1, 4, 1, 96, 0, 0, 3, 2, 1, 0, 10, 6, 1, 4, 0, 18, 0, 11],
  exceptions: [0, 97, 115, 109, 1, 0, 0, 0, 1, 4, 1, 96, 0, 0, 3, 2, 1, 0, 10, 8, 1, 6, 0, 6, 64, 25, 11, 11],
};

export type WasmFeature = "simd" | "threads" | "tailCall" | "exceptions";
export type WasmFeatures = Record<WasmFeature, boolean>;

export function detectWasmFeature(feature: WasmFeature): boolean {
  if (typeof WebAssembly !== "object") return false;
  try {
    return WebAssembly.validate(new Uint8Array(MODULES[feature]));
  } catch {
    return false;
  }
}

export function detectWasmFeatures(): WasmFeatures {
  return {
    simd: detectWasmFeature("simd"),
    threads: detectWasmFeature("threads"),
    tailCall: detectWasmFeature("tailCall"),
    exceptions: detectWasmFeature("exceptions"),
  };
}

// A shared memory that can grow to the runtime's 2 GB maximum.
export function sharedMemoryGrows(): boolean {
  try {
    const memory = new WebAssembly.Memory({ initial: 1, maximum: 32768, shared: true });
    return memory.grow(1) === 1;
  } catch {
    return false;
  }
}
