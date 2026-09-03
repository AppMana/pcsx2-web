// Replays every fixture GS dump through the wasm build's WebGPU renderer in
// hardware Chrome and compares the frames it reads back against the native
// Dawn renders of the same dumps (tests/fixtures/<name>/expected/webgpu-native/,
// written by web/scripts/webgpu-native-baseline.sh with the same
// GSDumpReplayer loop count). Both sides run the identical GSDeviceWebGPU
// code on the same GPU, so the comparison is an exact MD5 over the decoded
// pixels; the RMSE report is attached for diagnosis when it is not.
//
// Dumps need no BIOS. The lane needs a physical adapter: a software adapter
// fails the run rather than silently comparing SwiftShader output.
import { existsSync, readdirSync, readFileSync, writeFileSync } from "node:fs";
import path from "node:path";
import { expect, test } from "@playwright/test";
import { compareMd5, encodePng, pngMd5, pngRmse, rgbaFromResult } from "@appmana-public/web-emulator-harness/compare";
import { adapterIdentity, isSoftwareAdapter, probeWebGpuCapabilities } from "@appmana-public/web-emulator-harness/playwright";
import { validateReport } from "@appmana-public/web-emulator-harness/report";
import { discoverFixtures } from "../support/fixtures";

const RUN_TIMEOUT_MS = Number(process.env.PCSX2_RUN_TIMEOUT_MS ?? 180_000);
const LOOPS = 2;
const GS_HOST = process.env.PCSX2_GS_HOST === "main" ? "main" : "worker";

type DumpCase = {
  fixture: string;
  name: string;
  targetUrl: string;
  nativeDir: string;
  /** dump frame number (the native runner's frameNNNNN suffix) -> PNG path */
  nativeFrames: Map<number, string>;
};

function discoverDumps(): DumpCase[] {
  const cases: DumpCase[] = [];
  for (const fixture of discoverFixtures()) {
    const dumpsDir = path.join(fixture.expectedDir, "dumps");
    if (!existsSync(dumpsDir)) continue;
    for (const entry of readdirSync(dumpsDir).sort()) {
      if (!entry.endsWith(".gs.zst")) continue;
      const name = entry.slice(0, -".gs.zst".length);
      const nativeDir = path.join(fixture.expectedDir, "webgpu-native", name);
      const nativeFrames = new Map<number, string>();
      if (existsSync(nativeDir)) {
        for (const png of readdirSync(nativeDir)) {
          const match = new RegExp(`^${name}_frame(\\d+)\\.png$`).exec(png);
          if (match) nativeFrames.set(Number(match[1]), path.join(nativeDir, png));
        }
      }
      cases.push({
        fixture: fixture.name,
        name,
        targetUrl: path.posix.join("tests/fixtures", fixture.name, "expected", "dumps", entry),
        nativeDir,
        nativeFrames,
      });
    }
  }
  return cases;
}

const dumps = discoverDumps();

type CapturedFrame = {
  index: number;
  gpu?: { width?: number; height?: number; frameHash?: string; changedPixels?: number; dumpFrame?: number; dumpLoop?: number; rgbaBase64?: string };
  hostTimings?: { renderMs?: number };
};

test.describe.configure({ mode: "serial" });

test.beforeAll(async ({ browser }) => {
  const page = await browser.newPage();
  await page.goto("/runtime.html");
  // The first requestAdapter() of a fresh headless Chrome on this box returns null ("A valid
  // external Instance reference no longer exists"); the next one returns the hardware adapter.
  let capabilities = await page.evaluate(probeWebGpuCapabilities);
  for (let attempt = 0; attempt < 3 && !capabilities.webGpu; attempt++) {
    await page.waitForTimeout(250);
    capabilities = await page.evaluate(probeWebGpuCapabilities);
  }
  await page.close();
  test.skip(!capabilities.webGpu, "WebGPU is unavailable in this Chrome");
  expect(isSoftwareAdapter(capabilities), `hardware adapter required, got ${adapterIdentity(capabilities.adapter) || "none"}`).toBe(false);
});

test.skip(dumps.length === 0, "no fixture has expected/dumps/*.gs.zst");

for (const dump of dumps) {
  test(`${dump.fixture}/${dump.name}: browser WebGPU replay matches the native Dawn render (MD5)`, async ({ page }, testInfo) => {
    test.setTimeout(RUN_TIMEOUT_MS + 60_000);
    expect(dump.nativeFrames.size, `no native baseline under ${dump.nativeDir}; run web/scripts/webgpu-native-baseline.sh`).toBeGreaterThan(0);

    await page.goto("/runtime.html");
    await page.waitForFunction(() => Boolean((window as any).__pcsx2Runtime));
    const options = {
      frames: 100_000,
      render: true,
      renderer: "webgpu",
      gsHost: GS_HOST,
      readback: "async",
      captureRgba: true,
      captureEvery: 1,
      loops: LOOPS,
      timeoutMs: RUN_TIMEOUT_MS,
    };
    const report = await page.evaluate(async ({ target, options }) => {
      try {
        return await (window as any).__pcsx2Runtime.run(target, options);
      } catch (error) {
        return { schema: 1, emulator: "pcsx2", ok: false, detail: error instanceof Error ? `${error.name}: ${error.message}` : String(error), frames: [], events: [], tty: [], emu: {} };
      }
    }, { target: dump.targetUrl, options });

    const frames: CapturedFrame[] = report.frames ?? [];
    const captured = frames.filter((frame) => frame.gpu?.rgbaBase64);
    const evidence = async (name: string, body: string | Uint8Array, contentType: string) => {
      const file = testInfo.outputPath(name);
      writeFileSync(file, body);
      await testInfo.attach(name, { path: file, contentType });
    };
    // The report minus the pixel payloads is the readable evidence; the pixels become PNGs.
    const slim = { ...report, frames: frames.map((frame) => ({ ...frame, gpu: frame.gpu ? { ...frame.gpu, rgbaBase64: undefined } : undefined })) };
    await evidence(`${dump.fixture}-${dump.name}.report.json`, JSON.stringify(slim, null, 2), "application/json");
    for (const frame of captured) {
      const rgba = rgbaFromResult({ gpu: frame.gpu! });
      await evidence(`${dump.fixture}-${dump.name}-loop${frame.gpu!.dumpLoop}-frame${String(frame.gpu!.dumpFrame).padStart(5, "0")}.png`, encodePng(rgba), "image/png");
    }

    const validation = validateReport(report);
    expect(validation.errors, "report schema").toEqual([]);
    expect(report.ok, `run: ${report.detail}\nlogs: ${JSON.stringify(report.emu?.logs?.slice(-20) ?? [])}`).toBe(true);
    expect(report.gpu?.adapter, "report.gpu.adapter").toMatch(/WebGPU/);
    expect(isSoftwareAdapter({ webGpu: true, adapter: { vendor: "", architecture: "", device: report.gpu.adapter, description: "", isFallbackAdapter: false, features: [] } }), `software adapter in the module: ${report.gpu.adapter}`).toBe(false);

    // The last replay (dump loop 0) is what the native runner wrote to -dumpdir.
    const lastLoop = captured.filter((frame) => frame.gpu!.dumpLoop === 0);
    expect(lastLoop.length, `frames captured in the last replay (all captured: ${captured.map((f) => `${f.gpu!.dumpLoop}/${f.gpu!.dumpFrame}`).join(" ")})`).toBeGreaterThan(0);

    const lines: string[] = [`${dump.fixture}/${dump.name}: adapter ${report.gpu.adapter.split("\n").join(" | ")}`];
    const failures: string[] = [];
    for (const [dumpFrame, nativePng] of [...dump.nativeFrames.entries()].sort((a, b) => a[0] - b[0])) {
      const browserFrame = lastLoop.find((frame) => frame.gpu!.dumpFrame === dumpFrame);
      if (!browserFrame) {
        failures.push(`native frame ${dumpFrame} was not captured in the browser`);
        continue;
      }
      const native = new Uint8Array(readFileSync(nativePng));
      const expected = pngMd5(native);
      const actual = rgbaFromResult({ gpu: browserFrame.gpu! });
      const verdict = compareMd5(expected.md5, actual);
      let rmseText = "";
      if (!verdict.ok) {
        try {
          const rmse = pngRmse(native, actual);
          rmseText = ` rmse=${rmse.rmse.toFixed(4)} close=${(rmse.closePixelFraction * 100).toFixed(2)}% maxErr=${rmse.maxChannelError}`;
        } catch (error) {
          rmseText = ` (${error instanceof Error ? error.message : String(error)})`;
        }
        failures.push(`frame ${dumpFrame}: md5 ${verdict.actual} != native ${verdict.expected} (${expected.width}x${expected.height} vs ${actual.width}x${actual.height})${rmseText}`);
      }
      lines.push(`  frame ${String(dumpFrame).padStart(5, "0")}: ${verdict.ok ? "MD5 match" : "MISMATCH"} ${verdict.actual} (${actual.width}x${actual.height}) render ${browserFrame.hostTimings?.renderMs?.toFixed(2) ?? "?"} ms changed ${browserFrame.gpu!.changedPixels}${rmseText}`);
    }
    process.stdout.write(`${lines.join("\n")}\n`);
    expect(failures, lines.join("\n")).toEqual([]);
  });
}
