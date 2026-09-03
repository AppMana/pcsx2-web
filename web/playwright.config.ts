import { defineConfig } from "@playwright/test";
import { defineHarnessConfig } from "@appmana-public/web-emulator-harness/playwright";

// Specs that need a physical WebGPU adapter run in the hardware lane
// (playwright.gpu.config.ts); the default lane ignores them.
export const GPU_SPECS = ["elf-webgpu.spec.ts", "frame-oracle.spec.ts", "*-gpu.spec.ts"];

export const harness = defineHarnessConfig({
  apiGlobal: "__pcsx2Runtime",
  previewPort: 4193,
  gpuPreviewPort: 4194,
  libraryPort: 4195,
  libraryDir: "tests/fixtures",
  // The fixture ELFs live one directory down; scripts/serve-library.mjs
  // collects them for the kit's server.
  libraryCommand: "yarn serve-library",
  gpuSpecs: GPU_SPECS,
  executablePath: "/usr/bin/google-chrome",
  headedEnv: "PCSX2_HEADED",
  timeoutMs: 60_000,
  gpuTimeoutMs: 180_000,
});

export default defineConfig(harness.default);
