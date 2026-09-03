import { expect, test } from "@playwright/test";
import { pageApiProbe } from "@appmana-public/web-emulator-harness/contract";
import { probeWebGpuCapabilities } from "@appmana-public/web-emulator-harness/playwright";

test("publishes a machine-readable capability report", async ({ page }, testInfo) => {
  await page.goto("/");
  await expect(page.locator("h1")).toContainText("PCSX2 in the browser");
  const report = await page.evaluate(() => (window as Window & { __pcsx2Capabilities?: Promise<any> }).__pcsx2Capabilities);
  await testInfo.attach("capabilities.json", { body: JSON.stringify(report, null, 2), contentType: "application/json" });
  expect(report.schema).toBe(1);
  expect(report.crossOriginIsolated).toBe(true);
  expect(report.sharedArrayBuffer).toBe(true);
  expect(report.sharedMemoryGrows).toBe(true);
  expect(report.wasm.simd).toBe(true);
  expect(report.wasm.threads).toBe(true);
  expect(typeof report.wasm.tailCall).toBe("boolean");
  expect(report.atomicsWaitAsync).toBe(true);
  expect(report.dedicatedWorker).toBe(true);
  expect(report.opfs).toBe(true);
  expect(typeof report.webGpu.webGpu).toBe("boolean");
  await expect(page.locator("#evidence")).toContainText('"schema": 1');

  // The same probe the hardware lane evaluates, run from the test side.
  const kitProbe = await page.evaluate(probeWebGpuCapabilities);
  expect(kitProbe.crossOriginIsolated).toBe(true);
  expect(kitProbe.sharedArrayBuffer).toBe(true);
  expect(kitProbe.webGpu).toBe(report.webGpu.webGpu);
});

test("runtime.html exposes the page API contract", async ({ page }, testInfo) => {
  await page.goto("/runtime.html");
  await page.waitForFunction(pageApiProbe(testInfo.project.metadata.apiGlobal ?? "__pcsx2Runtime"));
  const stopped = await page.evaluate(() => (window as any).__pcsx2Runtime.stop());
  expect(stopped).toBeUndefined();
  const trace = await page.evaluate(() => (window as any).__pcsx2Runtime.exportInputTrace());
  expect(trace).toEqual({ schema: 1, entries: [], applied: 0 });
  await expect(page.locator("#status")).toHaveText("idle");
});
