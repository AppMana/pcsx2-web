// Boots every fixture with a recorded native oracle through the wasm runtime
// (software path, null renderer) and compares the guest TTY and, when the
// build exports them, the per frame CPU hash trace and the per frame audio
// hash trace against expected/. A fixture with an input recording
// (test.toml `input`) gets it replayed through the host's pad schedule,
// converted from the same .p2m2 the oracle replayed.
//
// The BIOS comes from PCSX2_BIOS (a path to the .bin); it is imported once
// into a persistent browser profile's origin-private storage through
// storage.html, which is the same path a user takes. Without PCSX2_BIOS the
// spec is skipped (CI lanes are BIOS-free).
import { readFileSync, writeFileSync } from "node:fs";
import { chromium, expect, test as base, type BrowserContext } from "@playwright/test";
import { judgeKnownFailure, selfBaselinePath } from "@appmana-public/web-emulator-harness/known-failure";
import { validateReport } from "@appmana-public/web-emulator-harness/report";
import { biosPath, ensureStoredBios, profileDir, storedBios } from "../support/bios";
import { compareAudio, compareCpu, compareTty, discoverFixtures, inputTraceFromP2m2, summarizeRun, type CpuVerdict, type Fixture, type TtyVerdict } from "../support/fixtures";

// Disc image fixtures boot from origin-private storage in disc-boot.spec.ts.
const fixtures = discoverFixtures().filter((fixture) => fixture.kind === "elf" && fixture.expectedTtyPath);
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

test.beforeAll(async ({ profile }, workerInfo) => {
  await ensureStoredBios(profile, String(workerInfo.project.use.baseURL));
});

for (const fixture of fixtures) {
  const compared = ["TTY", ...(fixture.expectedCpuPath ? ["CPU trace"] : []), ...(fixture.expectedAudioPath ? ["audio trace"] : [])].join(", ");
  test(`${fixture.name}: ${compared} match the native oracle${fixture.inputPath ? " with the input recording replayed" : ""}`, async ({ profile }, testInfo) => {
    test.setTimeout(RUN_TIMEOUT_MS + 60_000);
    const baseURL = String(testInfo.project.use.baseURL);
    const page = await profile.newPage();
    await page.goto(new URL("runtime.html", baseURL).href);
    await page.waitForFunction(() => Boolean((window as any).__pcsx2Runtime));

    const inputTrace = fixture.inputPath ? inputTraceFromP2m2(new Uint8Array(readFileSync(fixture.inputPath)), fixture.config.kit.frames) : undefined;
    const options = {
      frames: fixture.config.kit.frames,
      render: false,
      cpu: fixture.config.kit.cpu,
      bios: storedBios,
      trace: { cpu: fixture.config.trace.cpu, ramEvery: fixture.config.trace.ramEvery, tty: fixture.config.trace.tty, audio: Boolean(fixture.expectedAudioPath) },
      timeoutMs: RUN_TIMEOUT_MS,
      inputTrace,
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
    if (report.emu?.audioJsonl) await evidence(`${fixture.name}.audio.jsonl`, report.emu.audioJsonl, "application/x-ndjson");

    const validation = validateReport(report);
    expect(validation.errors, "report schema").toEqual([]);

    const tty: TtyVerdict = compareTty(readFileSync(fixture.expectedTtyPath!, "utf8"), report.tty ?? [], fixture.config.ttyFilter);
    let cpu: CpuVerdict | undefined;
    if (fixture.expectedCpuPath && typeof report.emu?.cpuJsonl === "string") {
      cpu = compareCpu(readFileSync(fixture.expectedCpuPath, "utf8"), report.emu.cpuJsonl, fixture.config.kit.frames);
    }
    let audio: CpuVerdict | undefined;
    if (fixture.expectedAudioPath && typeof report.emu?.audioJsonl === "string") {
      audio = compareAudio(readFileSync(fixture.expectedAudioPath, "utf8"), report.emu.audioJsonl, fixture.config.kit.frames);
    }
    const summary = summarizeRun(fixture.name, report, { tty, cpu, audio });
    process.stdout.write(`${summary}\n`);

    // A replayed recording must have reached the guest: one application per port state per vsync.
    if (inputTrace?.length) {
      expect(report.emu?.inputTrace?.scheduled, "scheduled pad states").toBe(inputTrace.length);
      expect(report.emu?.inputTrace?.applied ?? 0, "pad applications").toBeGreaterThan(0);
    }

    const passed = Boolean(report.ok) && tty.ok && (cpu ? cpu.ok : true) && (audio ? audio.ok : true);
    const reason = !report.ok ? `run failed: ${report.detail}` : !tty.ok ? tty.reason : cpu && !cpu.ok ? cpu.reason : audio && !audio.ok ? audio.reason : undefined;
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
