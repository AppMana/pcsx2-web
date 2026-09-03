declare module "virtual:web-emulator-kit/webgpu-probe" {
  export const probeWebGpuCapabilities: () => Promise<import("@appmana-public/web-emulator-harness/playwright").WebGpuCapabilities>;
}
