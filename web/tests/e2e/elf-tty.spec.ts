// Boots every fixture with a recorded native oracle through the wasm runtime
// (software path, null renderer) and compares the guest TTY and, when the
// build exports it, the per frame CPU hash trace against expected/.
//
// The BIOS comes from PCSX2_BIOS (a path to the .bin); it is imported once
// into a persistent browser profile's origin-private storage through
// storage.html, which is the same path a user takes. Without PCSX2_BIOS the
// spec is skipped (CI lanes are BIOS-free).
import { readFileSync, writeFileSync } from "node:fs";
import os from "node:os";
import path from "node:path";
import { chromium, expect, test as base, type BrowserContext } from "@playwright/test";
import { judgeKnownFailure, selfBaselinePath } from "@appmana-public/web-emulator-harness/known-failure";
import { validateReport } from "@appmana-public/web-emulator-harness/report";
import { compareCpu, compareTty, discoverFixtures, summarizeRun, type CpuVerdict, type Fixture, type TtyVerdict } from "../support/fixtures";

const biosPath = process.env.PCSX2_BIOS;
const profileDir = process.env.PCSX2_CHROME_PROFILE ?? path.join(os.homedir(), ".cache", "pcsx2-web-playwright-profile");
const fixtures = discoverFixtures().filter((fixture) => fixture.expectedTtyPath);
const RUN_TIMEOUT_MS = Number(process.env.PCSX2_RUN_TIMEOUT_MS ?? 240_000);

const test = base.extend<{}, { profile: BrowserContext }>({
  profile: [async ({}, use) => {
    const context = await chromium.launchPersistentContext(profileDir, { headless: true, args: ["--no-sandbox"] });
    await use(context);
    await context.close();
  }, { scope: "worker" }],
});

test.skip(!biosPath, "PCSX2_BIOS is unset; the ELF fixtures need a BIOS dump");
test.skip(fixtures.length === 0, "no fixture has expected/tty.txt");

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
    const after = await page.evaluate((target) => (window as any).__pcsx2Storage.list().then((entries: Array<{ path: string; size?: number }>) => entries.find((entry) => entry.path === target)), storedBios);
    expect(after?.size).toBe(size);
  }
  await page.close();
});

for (const fixture of fixtures) {
  test(`${fixture.name}: TTY${fixture.expectedCpuPath ? " and CPU trace" : ""} match the native oracle`, async ({ profile }, testInfo) => {
    test.setTimeout(RUN_TIMEOUT_MS + 60_000);
    const baseURL = String(testInfo.project.use.baseURL);
    const page = await profile.newPage();
    await page.goto(new URL("runtime.html", baseURL).href);
    await page.waitForFunction(() => Boolean((window as any).__pcsx2Runtime));

    const options = {
      frames: fixture.config.kit.frames,
      render: false,
      cpu: fixture.config.kit.cpu,
      bios: storedBios,
      trace: { cpu: fixture.config.trace.cpu, ramEvery: fixture.config.trace.ramEvery, tty: fixture.config.trace.tty },
      timeoutMs: RUN_TIMEOUT_MS,
    };
    const report = await page.evaluate(async ({ target, options }) => {
      try {
        return await (window as any).__pcsx2Runtime.run(target, options);
      } catch (error) {
        return { schema: 1, emulator: "pcsx2", ok: false, detail: error instanceof Error ? `${error.name}: ${error.message}` : String(error), frames: [], events: [], tty: [], emu: {} };
      }
    }, { target: fixture.targetUrl, options });
    await page.close();
    // Evidence on disk under test-results/, attached by path so it survives the run.
    const evidence = async (name: string, body: string, contentType: string) => {
      const file = testInfo.outputPath(name);
      writeFileSync(file, body);
      await testInfo.attach(name, { path: file, contentType });
    };
    await evidence(`${fixture.name}.report.json`, JSON.stringify(report, null, 2), "application/json");
    if (report.tty?.length) await evidence(`${fixture.name}.tty.txt`, report.tty.join("\n") + "\n", "text/plain");
    if (report.emu?.cpuJsonl) await evidence(`${fixture.name}.cpu.jsonl`, report.emu.cpuJsonl, "application/x-ndjson");

    const validation = validateReport(report);
    expect(validation.errors, "report schema").toEqual([]);

    const tty: TtyVerdict = compareTty(readFileSync(fixture.expectedTtyPath!, "utf8"), report.tty ?? [], fixture.config.ttyFilter);
    let cpu: CpuVerdict | undefined;
    if (fixture.expectedCpuPath && typeof report.emu?.cpuJsonl === "string") {
      cpu = compareCpu(readFileSync(fixture.expectedCpuPath, "utf8"), report.emu.cpuJsonl, fixture.config.kit.frames);
    }
    const summary = summarizeRun(fixture.name, report, { tty, cpu });
    process.stdout.write(`${summary}\n`);

    const passed = Boolean(report.ok) && tty.ok && (cpu ? cpu.ok : true);
    const reason = !report.ok ? `run failed: ${report.detail}` : !tty.ok ? tty.reason : cpu && !cpu.ok ? cpu.reason : undefined;
    const knownFailure = fixture.config.kit.known_failure || Boolean(fixture.config.kit.compare.tty?.known_failure);
    const verdict = await judgeKnownFailure({
      knownFailure,
      passed,
      actual: `${tty.actualFiltered.join("\n")}\n`,
      baselinePath: selfBaselinePath(fixture.expectedTtyPath!),
      reason,
    });
    expect(verdict.ok, `${verdict.status}${"reason" in verdict ? `: ${verdict.reason}` : ""}\n${summary}`).toBe(true);
  });
}

export type { Fixture };
