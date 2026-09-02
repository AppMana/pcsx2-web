// Which optional WebGPU features the local hardware Chrome exposes (the
// desktop parity lane), for comparison with the iPad report.
//
//   NODE_PATH=<dir with playwright> node web/spikes/chrome-webgpu-features.mjs
import { writeFile } from "node:fs/promises";
import { createRequire } from "node:module";
const { chromium } = createRequire(import.meta.url)("playwright");

import { mkdtemp } from "node:fs/promises";
import { tmpdir } from "node:os";
import path from "node:path";
const profile = process.env.CHROME_PROFILE || await mkdtemp(path.join(tmpdir(), "chrome-webgpu-spike-"));
const browser = await chromium.launchPersistentContext(profile, {
  executablePath: process.env.CHROME || "/usr/bin/google-chrome",
  headless: process.env.HEADED !== "1",
  args: ["--no-sandbox", "--enable-unsafe-webgpu", "--enable-webgpu-developer-features", "--ignore-gpu-blocklist", "--enable-features=Vulkan", "--use-angle=vulkan"],
});
const page = await browser.newPage();
await page.route("http://localhost/spike", (route) => route.fulfill({ contentType: "text/html", body: "<!doctype html><title>spike</title>" }));
await page.goto("http://localhost/spike");
const result = await page.evaluate(async () => {
  const adapter = (await navigator.gpu?.requestAdapter()) ?? (await navigator.gpu?.requestAdapter({ powerPreference: "high-performance" }));
  if (!adapter) return { error: "no adapter", gpu: !!navigator.gpu, secure: isSecureContext, ua: navigator.userAgent };
  return { ua: navigator.userAgent, info: adapter.info ? { vendor: adapter.info.vendor, architecture: adapter.info.architecture, device: adapter.info.device, description: adapter.info.description, isFallbackAdapter: adapter.info.isFallbackAdapter } : null, features: [...adapter.features].sort(), limits: { maxTextureDimension2D: adapter.limits.maxTextureDimension2D, maxBufferSize: adapter.limits.maxBufferSize, maxStorageBufferBindingSize: adapter.limits.maxStorageBufferBindingSize, minUniformBufferOffsetAlignment: adapter.limits.minUniformBufferOffsetAlignment } };
});
await browser.close();
await writeFile(new URL("./chrome-webgpu-features-report.json", import.meta.url), JSON.stringify(result, null, 2));
console.log(JSON.stringify(result, null, 2));
