// Runs a fixture on the attached iPad from the hosted, cross-origin-isolated
// origin (default https://pcsx2.appmana.com, the nginx Ingress in front of
// this machine's preview server) through the kit's origin lane: the page's
// own API and the page's own build, so the report has the desktop lanes'
// shape. The lane navigates the device's current page to runtime.html and
// returns it to its original URL afterwards. ELF targets need the BIOS in
// the origin's storage; with PCSX2_BIOS set it is imported through
// storage.html's page API when missing.
//
//   node scripts/run-origin-device.mjs [target] [evidenceRoot]
//   target: <fixture> or <fixture>/<dump>, as for run-injected-device.mjs; default hello_tty
//   PCSX2_DEVICE_ORIGIN   origin to run from (default https://pcsx2.appmana.com/)
//   PCSX2_BIOS            BIOS image to import into the origin's storage when absent
//   PCSX2_DEVICE_RENDER=1 PCSX2_DEVICE_GS_HOST PCSX2_DEVICE_PTHREAD_POOL PCSX2_DEVICE_QUIET_MS
//   PCSX2_DEVICE_ADAPTER  PCSX2_DEVICE_KEEP_PAGE=1 (stay on runtime.html afterwards)
//   WIP_DISCOVERY_URL  WIP_TIMEOUT_MS
import path from "node:path";
import { DEFAULT_ADAPTER_PATTERN, DEFAULT_CURRENT_DIR, DEFAULT_EVIDENCE_ROOT, DEFAULT_ORIGIN, DEFAULT_PTHREAD_POOL_SIZE, DEFAULT_QUIET_MS, DEFAULT_TIMEOUT_MS, resolveDeviceTarget, runOriginDevice } from "./device-lane-support.mjs";

const target = resolveDeviceTarget(process.argv[2] || "hello_tty");
const evidenceRoot = path.resolve(process.argv[3] || DEFAULT_EVIDENCE_ROOT);
const log = (line) => process.stderr.write(`${line}\n`);

const outcome = await runOriginDevice({
  target,
  origin: process.env.PCSX2_DEVICE_ORIGIN || DEFAULT_ORIGIN,
  biosPath: process.env.PCSX2_BIOS,
  evidenceRoot,
  currentDir: DEFAULT_CURRENT_DIR,
  render: process.env.PCSX2_DEVICE_RENDER === "1",
  gsHost: process.env.PCSX2_DEVICE_GS_HOST === "main" ? "main" : process.env.PCSX2_DEVICE_GS_HOST === "worker" ? "worker" : undefined,
  pthreadPoolSize: Number(process.env.PCSX2_DEVICE_PTHREAD_POOL || DEFAULT_PTHREAD_POOL_SIZE),
  timeoutMs: Number(process.env.WIP_TIMEOUT_MS || DEFAULT_TIMEOUT_MS),
  discoveryURL: process.env.WIP_DISCOVERY_URL,
  quietMs: process.env.PCSX2_DEVICE_QUIET_MS === undefined ? DEFAULT_QUIET_MS : Number(process.env.PCSX2_DEVICE_QUIET_MS),
  adapterPattern: process.env.PCSX2_DEVICE_ADAPTER ? new RegExp(process.env.PCSX2_DEVICE_ADAPTER, "i") : DEFAULT_ADAPTER_PATTERN,
  restorePage: process.env.PCSX2_DEVICE_KEEP_PAGE !== "1",
  log,
});

if (outcome.skipped) {
  process.stdout.write(`${JSON.stringify({ skipped: true, reason: outcome.reason, checks: outcome.quiet?.checks }, null, 2)}\n`);
  process.exitCode = 2;
} else {
  process.stdout.write(`${JSON.stringify({ passed: outcome.passed, evidenceDir: outcome.evidenceDir, measurements: outcome.measurements, checks: outcome.verdict.checks, tty: outcome.verdict.tty, cpu: outcome.verdict.cpu, frames: outcome.verdict.frames, capabilities: outcome.capabilities, bios: outcome.bios }, null, 2)}\n`);
  if (!outcome.passed) process.exitCode = 1;
}
