// Boots gs_sprite.elf through the wasm build's WebGPU renderer in hardware
// Chrome, reads back the frames at the fixture's [compare.frames.webgpu]
// triggers and compares them with the native oracle's PNGs (recorded by
// pcsx2-tracerunner with the software renderer) using the RMSE and close
// pixel thresholds from test.toml. The oracle names its frames by
// g_FrameCount + 1 as posted to its GS thread; the browser host posts the
// same number, and the frame one vsync either side is also read back because
// both runners pick the request up at whichever vsync the GS thread handles
// next. The best of the three is judged and all are reported.
//
// Needs the BIOS from PCSX2_BIOS, imported once into the persistent profile
// through storage.html like elf-tty.spec.ts.
import { readFileSync, writeFileSync } from "node:fs";
import os from "node:os";
import path from "node:path";
import { chromium, expect, test as base, type BrowserContext } from "@playwright/test";
import { encodePng, pngRmse, rgbaFromResult } from "@appmana-public/web-emulator-harness/compare";
import { HARDWARE_WEBGPU_CHROME_ARGS, adapterIdentity, isSoftwareAdapter, probeWebGpuCapabilities } from "@appmana-public/web-emulator-harness/playwright";
import { validateReport } from "@appmana-public/web-emulator-harness/report";
import { discoverFixtures, summarizeRun } from "../support/fixtures";

const FIXTURE_NAME = process.env.PCSX2_ORACLE_FIXTURE ?? "gs_sprite";
const biosPath = process.env.PCSX2_BIOS;
const profileDir = process.env.PCSX2_CHROME_PROFILE ?? path.join(os.homedir(), ".cache", "pcsx2-web-playwright-profile");
const RUN_TIMEOUT_MS = Number(process.env.PCSX2_RUN_TIMEOUT_MS ?? 300_000);
const GS_HOST = process.env.PCSX2_GS_HOST === "main" ? "main" : "worker";
const fixture = discoverFixtures().find((entry) => entry.name === FIXTURE_NAME);

const test = base.extend<{}, { profile: BrowserContext }>({
  profile: [async ({}, use) => {
    const context = await chromium.launchPersistentContext(profileDir, { executablePath: "/usr/bin/google-chrome", headless: process.env.PCSX2_HEADED !== "1", args: [...HARDWARE_WEBGPU_CHROME_ARGS] });
    await use(context);
    await context.close();
  }, { scope: "worker" }],
});

test.skip(!biosPath, "PCSX2_BIOS is unset; the ELF oracle needs a BIOS dump");
test.skip(!fixture, `fixture ${FIXTURE_NAME} not found`);
test.skip(Boolean(fixture) && !fixture!.config.webgpuFrames, `${FIXTURE_NAME} has no [compare.frames.webgpu] section`);

const biosName = biosPath ? path.basename(biosPath) : "";
const storedBios = `pcsx2/bios/${biosName}`;

test.beforeAll(async ({ profile }, workerInfo) => {
  const baseURL = String(workerInfo.project.use.baseURL);
  const page = await profile.newPage();
  await page.goto(new URL("storage.html", baseURL).href);
  await page.waitForFunction(() => Boolean((window as any).__pcsx2Storage));
  const stored = await page.evaluate((target) => (window as any).__pcsx2Storage.list().then((entries: Array<{ path: string; size?: number }>) => entries.find((entry) => entry.path === target)), storedBios);
  const size = readFileSync(biosPath!).length;
  if (!stored || stored.size !== size) {
    await page.setInputFiles("#bios", biosPath!);
    const outcome = await page.evaluate(() => (window as any).__pcsx2StorageLastImport);
    expect(outcome, `BIOS import through storage.html: ${JSON.stringify(outcome)}`).toEqual(expect.objectContaining({ ok: true }));
  }
  let capabilities = await page.evaluate(probeWebGpuCapabilities);
  for (let attempt = 0; attempt < 3 && !capabilities.webGpu; attempt++) {
    await page.waitForTimeout(250);
    capabilities = await page.evaluate(probeWebGpuCapabilities);
  }
  await page.close();
  test.skip(!capabilities.webGpu, "WebGPU is unavailable in this Chrome");
  expect(isSoftwareAdapter(capabilities), `hardware adapter required, got ${adapterIdentity(capabilities.adapter) || "none"}`).toBe(false);
});

type CapturedFrame = {
  index: number;
  gpu?: { width?: number; height?: number; frameHash?: string; oracleFrame?: number; rgbaBase64?: string };
  hostTimings?: { renderMs?: number };
};

test(`${FIXTURE_NAME}: WebGPU frames match the native oracle within the fixture thresholds`, async ({ profile }, testInfo) => {
  test.setTimeout(RUN_TIMEOUT_MS + 60_000);
  const compare = fixture!.config.webgpuFrames!;
  const triggers = (compare.trigger ?? []).map((frame) => Number(frame));
  expect(triggers.length, "[compare.frames.webgpu] trigger").toBeGreaterThan(0);
  const captureFrames = [...new Set(triggers.flatMap((frame) => [frame - 1, frame, frame + 1]))].filter((frame) => frame >= 0);
  const frames = Math.max(fixture!.config.kit.frames, Math.max(...triggers) + 2);

  const baseURL = String(testInfo.project.use.baseURL);
  const page = await profile.newPage();
  await page.goto(new URL("runtime.html", baseURL).href);
  await page.waitForFunction(() => Boolean((window as any).__pcsx2Runtime));
  const options = {
    frames,
    render: true,
    renderer: "webgpu",
    gsHost: GS_HOST,
    readback: "async",
    captureRgba: true,
    captureEvery: 0,
    captureFrames,
    cpu: fixture!.config.kit.cpu,
    bios: storedBios,
    trace: { cpu: false, ramEvery: 0, tty: true },
    timeoutMs: RUN_TIMEOUT_MS,
  };
  const report = await page.evaluate(async ({ target, options }) => {
    try {
      return await (window as any).__pcsx2Runtime.run(target, options);
    } catch (error) {
      return { schema: 1, emulator: "pcsx2", ok: false, detail: error instanceof Error ? `${error.name}: ${error.message}` : String(error), frames: [], events: [], tty: [], emu: {} };
    }
  }, { target: fixture!.targetUrl, options });
  await page.close();

  const evidence = async (name: string, body: string | Uint8Array, contentType: string) => {
    const file = testInfo.outputPath(name);
    writeFileSync(file, body);
    await testInfo.attach(name, { path: file, contentType });
  };
  const allFrames: CapturedFrame[] = report.frames ?? [];
  const captured = allFrames.filter((frame) => frame.gpu?.rgbaBase64);
  const slim = { ...report, frames: allFrames.map((frame) => ({ ...frame, gpu: frame.gpu ? { ...frame.gpu, rgbaBase64: undefined } : undefined })) };
  await evidence(`${FIXTURE_NAME}.report.json`, JSON.stringify(slim, null, 2), "application/json");
  for (const frame of captured) {
    await evidence(`${FIXTURE_NAME}-oracle${String(frame.gpu!.oracleFrame).padStart(5, "0")}-present${frame.index}.png`, encodePng(rgbaFromResult({ gpu: frame.gpu! })), "image/png");
  }
  process.stdout.write(`${summarizeRun(FIXTURE_NAME, report)}\n`);

  expect(validateReport(report).errors, "report schema").toEqual([]);
  expect(report.ok, `run: ${report.detail}\nlogs: ${JSON.stringify(report.emu?.logs?.slice(-20) ?? [])}`).toBe(true);
  expect(report.gpu?.adapter, "report.gpu.adapter").toMatch(/WebGPU/);

  const lines: string[] = [`${FIXTURE_NAME}: adapter ${String(report.gpu.adapter).split("\n").join(" | ")}; thresholds max_rmse=${compare.max_rmse} min_close_pixels=${compare.min_close_pixels}`];
  const failures: string[] = [];
  for (const trigger of triggers) {
    const oraclePng = path.join(fixture!.expectedDir, "frames", `frame${String(trigger).padStart(5, "0")}.png`);
    const native = new Uint8Array(readFileSync(oraclePng));
    const candidates = captured.filter((frame) => Math.abs(Number(frame.gpu!.oracleFrame) - trigger) <= 1);
    if (!candidates.length) {
      failures.push(`frame ${trigger}: nothing captured near it (captured oracle frames: ${captured.map((frame) => frame.gpu!.oracleFrame).join(" ")})`);
      continue;
    }
    let best: { frame: CapturedFrame; rmse: ReturnType<typeof pngRmse> } | undefined;
    for (const frame of candidates) {
      try {
        const rmse = pngRmse(native, rgbaFromResult({ gpu: frame.gpu! }), { maxRmse: compare.max_rmse, minClosePixels: compare.min_close_pixels });
        lines.push(`  frame ${trigger} vs browser oracle frame ${frame.gpu!.oracleFrame} (present ${frame.index}): rmse=${rmse.rmse.toFixed(4)} close=${(rmse.closePixelFraction * 100).toFixed(2)}% exact=${(rmse.exactPixelFraction * 100).toFixed(2)}% maxErr=${rmse.maxChannelError} render ${frame.hostTimings?.renderMs?.toFixed(2) ?? "?"} ms`);
        if (!best || rmse.rmse < best.rmse.rmse) best = { frame, rmse };
      } catch (error) {
        lines.push(`  frame ${trigger} vs browser oracle frame ${frame.gpu!.oracleFrame}: ${error instanceof Error ? error.message : String(error)}`);
      }
    }
    if (!best) failures.push(`frame ${trigger}: no comparable capture`);
    else if (!best.rmse.ok) failures.push(`frame ${trigger}: best candidate (oracle frame ${best.frame.gpu!.oracleFrame}) rmse=${best.rmse.rmse.toFixed(4)} close=${(best.rmse.closePixelFraction * 100).toFixed(2)}% exceeds the thresholds`);
  }
  process.stdout.write(`${lines.join("\n")}\n`);
  expect(failures, lines.join("\n")).toEqual([]);
});
