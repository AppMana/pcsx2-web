// Transfers the local runtime build (web/dist) into an existing
// cross-origin-isolated page on the attached iPad as Blob URLs through the
// kit's injected lane, runs a fixture there through the page API, judges it
// against the native oracle in Node, writes the evidence, and removes the
// injection again. Nothing is navigated; the page keeps its URL.
//
//   node scripts/run-injected-device.mjs [target] [evidenceRoot]
//   target: <fixture> (an ELF with the null renderer, TTY and cpu.jsonl
//   verdict) or <fixture>/<dump> (a GS dump replayed through WebGPU,
//   frames against expected/webgpu-native/); default hello_tty
//   PCSX2_BIOS            BIOS image for ELF targets (uploaded with the build)
//   PCSX2_DEVICE_DIST     built runtime directory (default web/dist)
//   PCSX2_DEVICE_PAGE     substring of the page URL to inject into (default appmana.com)
//   PCSX2_DEVICE_RENDER=1 ELF targets: WebGPU instead of the null renderer
//   PCSX2_DEVICE_GS_HOST  worker | main (default: worker when navigator.gpu exists in a Worker)
//   PCSX2_DEVICE_PTHREAD_POOL  pthread pool size (default 6; the iPad reports 8 cores)
//   PCSX2_DEVICE_QUIET_MS spacing of the two page-list readings before touching the device
//                         (default 120000; 0 skips the wait, not the check)
//   PCSX2_DEVICE_ADAPTER  regex the adapter identity must match (default apple)
//   WIP_DISCOVERY_URL  WIP_TIMEOUT_MS
import path from "node:path";
import { DEFAULT_ADAPTER_PATTERN, DEFAULT_CURRENT_DIR, DEFAULT_DIST, DEFAULT_EVIDENCE_ROOT, DEFAULT_PAGE_MATCH, DEFAULT_PTHREAD_POOL_SIZE, DEFAULT_QUIET_MS, DEFAULT_TIMEOUT_MS, resolveDeviceTarget, runInjectedDevice } from "./device-lane-support.mjs";

const target = resolveDeviceTarget(process.argv[2] || "hello_tty");
const evidenceRoot = path.resolve(process.argv[3] || DEFAULT_EVIDENCE_ROOT);
const log = (line) => process.stderr.write(`${line}\n`);

const outcome = await runInjectedDevice({
  target,
  distDir: process.env.PCSX2_DEVICE_DIST ? path.resolve(process.env.PCSX2_DEVICE_DIST) : DEFAULT_DIST,
  biosPath: process.env.PCSX2_BIOS,
  pageMatch: process.env.PCSX2_DEVICE_PAGE || DEFAULT_PAGE_MATCH,
  evidenceRoot,
  currentDir: DEFAULT_CURRENT_DIR,
  render: process.env.PCSX2_DEVICE_RENDER === "1",
  gsHost: process.env.PCSX2_DEVICE_GS_HOST === "main" ? "main" : process.env.PCSX2_DEVICE_GS_HOST === "worker" ? "worker" : undefined,
  pthreadPoolSize: Number(process.env.PCSX2_DEVICE_PTHREAD_POOL || DEFAULT_PTHREAD_POOL_SIZE),
  timeoutMs: Number(process.env.WIP_TIMEOUT_MS || DEFAULT_TIMEOUT_MS),
  discoveryURL: process.env.WIP_DISCOVERY_URL,
  quietMs: process.env.PCSX2_DEVICE_QUIET_MS === undefined ? DEFAULT_QUIET_MS : Number(process.env.PCSX2_DEVICE_QUIET_MS),
  adapterPattern: process.env.PCSX2_DEVICE_ADAPTER ? new RegExp(process.env.PCSX2_DEVICE_ADAPTER, "i") : DEFAULT_ADAPTER_PATTERN,
  log,
});

if (outcome.skipped) {
  process.stdout.write(`${JSON.stringify({ skipped: true, reason: outcome.reason, checks: outcome.quiet?.checks }, null, 2)}\n`);
  process.exitCode = 2;
} else {
  process.stdout.write(`${JSON.stringify({ passed: outcome.passed, evidenceDir: outcome.evidenceDir, measurements: outcome.measurements, checks: outcome.verdict.checks, tty: outcome.verdict.tty, cpu: outcome.verdict.cpu, frames: outcome.verdict.frames, capabilities: outcome.capabilities, cleanup: outcome.cleanup }, null, 2)}\n`);
  if (!outcome.passed) process.exitCode = 1;
}
