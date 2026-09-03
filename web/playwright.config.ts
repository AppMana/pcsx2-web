import { defineConfig } from "@playwright/test";
import { defineHarnessConfig } from "@appmana-public/web-emulator-harness/playwright";

// Specs that need a physical WebGPU adapter run in the hardware lane
// (playwright.gpu.config.ts); the default lane ignores them.
export const GPU_SPECS = ["elf-webgpu.spec.ts", "frame-oracle.spec.ts", "*-gpu.spec.ts"];

// PCSX2_PREVIEW_PORT, PCSX2_GPU_PREVIEW_PORT and PCSX2_LIBRARY_PORT move the
// three servers so that a second checkout can run the suite at the same time.
const previewPort = Number(process.env.PCSX2_PREVIEW_PORT ?? 4193);
const gpuPreviewPort = Number(process.env.PCSX2_GPU_PREVIEW_PORT ?? 4194);
const libraryPort = Number(process.env.PCSX2_LIBRARY_PORT ?? 4195);
// The preview server (vite.config.ts) proxies /library/ to the library port.
process.env.PCSX2_LIBRARY_PROXY ??= `http://127.0.0.1:${libraryPort}`;

export const harness = defineHarnessConfig({
  apiGlobal: "__pcsx2Runtime",
  previewPort,
  gpuPreviewPort,
  libraryPort,
  libraryDir: "tests/fixtures",
  // The fixture ELFs live one directory down; scripts/serve-library.mjs
  // collects them for the kit's server.
  libraryCommand: `yarn serve-library --port ${libraryPort}`,
  gpuSpecs: GPU_SPECS,
  executablePath: "/usr/bin/google-chrome",
  headedEnv: "PCSX2_HEADED",
  timeoutMs: 60_000,
  gpuTimeoutMs: 180_000,
});

export default defineConfig(harness.default);
