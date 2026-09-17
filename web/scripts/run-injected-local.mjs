// The injected lane without an iPad: the same upload, page driver, judges,
// evidence and cleanup as run-injected-device.mjs, but the "device" is a
// local headless Chrome on a page of this checkout's preview server
// (cross-origin isolated by its COOP/COEP headers). Proves the Blob URL
// module graph, the pthread worker rewrite and the page API wiring before
// a device run, and gives the desktop reference for the measurements.
//
//   node scripts/run-injected-local.mjs [target] [evidenceRoot]
//   PCSX2_BIOS  PCSX2_DEVICE_DIST  PCSX2_DEVICE_RENDER=1  PCSX2_DEVICE_GS_HOST
//   PCSX2_DEVICE_PTHREAD_POOL  PCSX2_LOCAL_PORT (default 4196)  PCSX2_HEADED=1
import { spawn } from "node:child_process";
import path from "node:path";
import { chromium } from "playwright";
import { HARDWARE_WEBGPU_CHROME_ARGS } from "@appmana-public/web-emulator-harness/playwright";
import { delay } from "@appmana-public/web-emulator-harness/webkit-inspector";
import { DEFAULT_CURRENT_DIR, DEFAULT_DIST, DEFAULT_EVIDENCE_ROOT, DEFAULT_PTHREAD_POOL_SIZE, DEFAULT_TIMEOUT_MS, WEB_ROOT, resolveDeviceTarget, runInjectedDevice } from "./device-lane-support.mjs";

const target = resolveDeviceTarget(process.argv[2] || "hello_tty");
const evidenceRoot = path.resolve(process.argv[3] || DEFAULT_EVIDENCE_ROOT);
const port = Number(process.env.PCSX2_LOCAL_PORT || 4196);
const distDir = process.env.PCSX2_DEVICE_DIST ? path.resolve(process.env.PCSX2_DEVICE_DIST) : DEFAULT_DIST;
const log = (line) => process.stderr.write(`${line}\n`);
const pageURL = `http://127.0.0.1:${port}/units.html`;

const preview = spawn("yarn", ["vite", "preview", "--port", String(port), "--strictPort", "--outDir", distDir], { cwd: WEB_ROOT, stdio: ["ignore", "ignore", "inherit"] });
try {
  for (let attempt = 0; ; attempt += 1) {
    const ready = await fetch(pageURL).then((response) => response.ok).catch(() => false);
    if (ready) break;
    if (attempt > 100) throw new Error(`preview server did not answer on ${pageURL}`);
    await delay(200);
  }
  const browser = await chromium.launch({ executablePath: "/usr/bin/google-chrome", headless: process.env.PCSX2_HEADED !== "1", args: [...HARDWARE_WEBGPU_CHROME_ARGS] });
  try {
    const page = await browser.newPage();
    page.on("console", (message) => { if (message.type() === "error") log(`page console: ${message.text()}`); });
    await page.goto(pageURL);
    const device = { deviceName: "local Chrome", deviceOSVersion: browser.version(), url: `127.0.0.1:${port}` };
    const pages = [{ url: pageURL, title: "units", webSocketDebuggerUrl: "" }];
    const connection = {
      // Playwright awaits a promise-valued expression on its own.
      evaluate: (expression) => page.evaluate(expression),
      command: async () => ({}),
      snapshotPng: () => page.screenshot(),
      close: () => {},
    };
    const outcome = await runInjectedDevice({
      target,
      distDir,
      biosPath: process.env.PCSX2_BIOS,
      evidenceRoot,
      currentDir: path.join(DEFAULT_CURRENT_DIR, "local"),
      render: process.env.PCSX2_DEVICE_RENDER === "1",
      gsHost: process.env.PCSX2_DEVICE_GS_HOST === "main" ? "main" : process.env.PCSX2_DEVICE_GS_HOST === "worker" ? "worker" : undefined,
      pthreadPoolSize: Number(process.env.PCSX2_DEVICE_PTHREAD_POOL || DEFAULT_PTHREAD_POOL_SIZE),
      timeoutMs: Number(process.env.WIP_TIMEOUT_MS || DEFAULT_TIMEOUT_MS),
      quietMs: 0,
      discover: async () => ({ device, pages }),
      processes: () => [],
      connect: async () => ({ device, page: pages[0], connection }),
      adapterPattern: /./,
      log,
    });
    process.stdout.write(`${JSON.stringify({ passed: outcome.passed, evidenceDir: outcome.evidenceDir, measurements: outcome.measurements, checks: outcome.verdict?.checks, tty: outcome.verdict?.tty, cpu: outcome.verdict?.cpu, frames: outcome.verdict?.frames, capabilities: outcome.capabilities, cleanup: outcome.cleanup }, null, 2)}\n`);
    if (!outcome.passed) process.exitCode = 1;
  } finally {
    await browser.close();
  }
} finally {
  preview.kill();
}
