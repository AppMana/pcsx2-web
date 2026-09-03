// Plays the pad_echo fixture (it streams a tone to the SPU2) with audio on:
// the SPU2 output reaches an audio worklet on the page's AudioContext, and
// the run must show frames actually pulled by the worklet, non-silent ones
// among them, a non-zero level on the page's AnalyserNode, and per vsync
// audio hashes identical to the native oracle's audio.jsonl.
//
// Headless Chromium needs --autoplay-policy=no-user-gesture-required for an
// AudioContext created without a gesture to run; Playwright's default
// --mute-audio is dropped so the graph renders into a real output sink.
import { readFileSync, writeFileSync } from "node:fs";
import { chromium, expect, test as base, type BrowserContext } from "@playwright/test";
import { validateReport } from "@appmana-public/web-emulator-harness/report";
import { biosPath, ensureStoredBios, profileDir, storedBios } from "../support/bios";
import { compareAudio, discoverFixtures, inputTraceFromP2m2, summarizeRun } from "../support/fixtures";

const FIXTURE_NAME = "pad_echo";
const FRAMES = 250;
const fixture = discoverFixtures().find((entry) => entry.name === FIXTURE_NAME);
const RUN_TIMEOUT_MS = Number(process.env.PCSX2_RUN_TIMEOUT_MS ?? 240_000);

const test = base.extend<{}, { profile: BrowserContext }>({
  profile: [async ({}, use) => {
    const context = await chromium.launchPersistentContext(profileDir, {
      headless: true,
      args: ["--no-sandbox", "--autoplay-policy=no-user-gesture-required"],
      ignoreDefaultArgs: ["--mute-audio"],
    });
    await use(context);
    await context.close();
  }, { scope: "worker" }],
});

test.skip(!biosPath, "PCSX2_BIOS is unset; the ELF fixtures need a BIOS dump");
test.skip(!fixture?.expectedAudioPath, `${FIXTURE_NAME} has no recorded audio.jsonl`);

test.beforeAll(async ({ profile }, workerInfo) => {
  await ensureStoredBios(profile, String(workerInfo.project.use.baseURL));
});

test(`${FIXTURE_NAME}: the audio worklet pulls non-silent SPU2 output and the audio hashes match the oracle`, async ({ profile }, testInfo) => {
  test.setTimeout(RUN_TIMEOUT_MS + 60_000);
  const baseURL = String(testInfo.project.use.baseURL);
  const page = await profile.newPage();
  await page.goto(new URL("runtime.html", baseURL).href);
  await page.waitForFunction(() => Boolean((window as any).__pcsx2Runtime));

  const inputTrace = fixture!.inputPath ? inputTraceFromP2m2(new Uint8Array(readFileSync(fixture!.inputPath)), FRAMES) : undefined;
  const options = {
    frames: FRAMES,
    render: false,
    cpu: fixture!.config.kit.cpu,
    bios: storedBios,
    trace: { cpu: false, ramEvery: 0, tty: true, audio: true },
    timeoutMs: RUN_TIMEOUT_MS,
    inputTrace,
    audio: true,
  };
  const report = await page.evaluate(async ({ target, options }) => {
    try {
      return await (window as any).__pcsx2Runtime.run(target, options);
    } catch (error) {
      return { schema: 1, emulator: "pcsx2", ok: false, detail: error instanceof Error ? `${error.name}: ${error.message}` : String(error), frames: [], events: [], tty: [], emu: {} };
    }
  }, { target: fixture!.targetUrl, options });
  await page.close();

  const evidence = async (name: string, body: string, contentType: string) => {
    const file = testInfo.outputPath(name);
    writeFileSync(file, body);
    await testInfo.attach(name, { path: file, contentType });
  };
  await evidence(`${FIXTURE_NAME}.audio.report.json`, JSON.stringify(report, null, 2), "application/json");
  if (report.emu?.audioJsonl) await evidence(`${FIXTURE_NAME}.audio.jsonl`, report.emu.audioJsonl, "application/x-ndjson");

  expect(validateReport(report).errors, "report schema").toEqual([]);
  const audioVerdict = typeof report.emu?.audioJsonl === "string" ? compareAudio(readFileSync(fixture!.expectedAudioPath!, "utf8"), report.emu.audioJsonl, FRAMES) : undefined;
  const summary = summarizeRun(FIXTURE_NAME, report, { audio: audioVerdict });
  process.stdout.write(`${summary}\n`);

  expect(report.ok, `run: ${report.detail}\n${summary}`).toBe(true);
  const audio = report.emu?.audio ?? {};
  expect(audio.requested, "audio requested").toBe(true);
  expect(audio.attached, `worklet attached: ${JSON.stringify(audio)}`).toBe(true);
  const worklet = audio.worklet ?? {};
  expect(worklet.stateName, "worklet state").toBe("ready");
  expect(worklet.sampleRate, "context sample rate").toBe(48000);
  expect(worklet.callbacks, "process() calls").toBeGreaterThan(0);
  expect(worklet.pulledFrames, "frames pulled by the worklet").toBeGreaterThan(0);
  expect(worklet.nonzeroFrames, "non-silent frames pulled by the worklet").toBeGreaterThan(0);
  expect(worklet.writtenFrames, "frames the SPU2 wrote").toBeGreaterThan(0);
  const pageStats = audio.page ?? {};
  expect(pageStats.errors, "page audio errors").toEqual([]);
  expect(pageStats.contextState, "AudioContext state").toBe("running");
  expect(pageStats.workletBooted, "worklet booted").toBe(true);
  expect(pageStats.nonzeroSamples, "non-zero samples seen by the page's AnalyserNode").toBeGreaterThan(0);
  expect(pageStats.peak, "peak level on the page").toBeGreaterThan(0);
  expect(audioVerdict, "audio.jsonl present").toBeDefined();
  expect(audioVerdict!.ok, `${audioVerdict!.reason}\n${summary}`).toBe(true);
});
