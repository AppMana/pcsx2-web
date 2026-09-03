// Boots the disc image fixtures from origin-private storage: every ISO and CHD
// of a disc fixture is imported from the same-origin fixture library on
// storage.html (the kit's resumable importer), then run on runtime.html as
// "/opfs/games/<image>" with the null renderer, and the guest TTY and the per
// frame CPU hash trace are compared against the fixture's recorded native
// oracle exactly like elf-tty.spec.ts does. The core reads the image in place
// through pcsx2/CDVD/OpfsFileReader.cpp; the report carries the sync access
// handle mode the browser granted.
//
// A last test enables a memory card for a short run and checks that the card
// created in MEMFS is written back to storage on stop and restored by the next
// run (runtime-worker.mjs persistence).
//
// Needs PCSX2_BIOS like elf-tty.spec.ts; skipped without it.
import { readFileSync, writeFileSync } from "node:fs";
import os from "node:os";
import path from "node:path";
import { chromium, expect, test as base, type BrowserContext, type Page } from "@playwright/test";
import { judgeKnownFailure, selfBaselinePath } from "@appmana-public/web-emulator-harness/known-failure";
import { validateReport } from "@appmana-public/web-emulator-harness/report";
import { compareCpu, compareTty, discoverFixtures, summarizeRun, type CpuVerdict, type Fixture, type FixtureImage, type TtyVerdict } from "../support/fixtures";

const biosPath = process.env.PCSX2_BIOS;
const profileDir = process.env.PCSX2_CHROME_PROFILE ?? path.join(os.homedir(), ".cache", "pcsx2-web-playwright-profile");
const fixtures = discoverFixtures().filter((fixture) => fixture.kind === "disc" && fixture.expectedTtyPath);
const RUN_TIMEOUT_MS = Number(process.env.PCSX2_RUN_TIMEOUT_MS ?? 240_000);
const GAMES_DIR = "games";
const MEMCARD = "Mcd001.ps2";
const STORED_MEMCARD = `pcsx2/memcards/${MEMCARD}`;

const test = base.extend<{}, { profile: BrowserContext }>({
  profile: [async ({}, use) => {
    const context = await chromium.launchPersistentContext(profileDir, { headless: true, args: ["--no-sandbox"] });
    await use(context);
    await context.close();
  }, { scope: "worker" }],
});

test.skip(!biosPath, "PCSX2_BIOS is unset; the disc fixtures need a BIOS dump");
test.skip(fixtures.length === 0, "no disc fixture has expected/tty.txt");

const biosName = biosPath ? path.basename(biosPath) : "";
const storedBios = `pcsx2/bios/${biosName}`;

type StoredEntry = { path: string; size?: number; import?: { complete: boolean; sha256?: string } };

async function storagePage(profile: BrowserContext, baseURL: string): Promise<Page> {
  const page = await profile.newPage();
  await page.goto(new URL("storage.html", baseURL).href);
  await page.waitForFunction(() => Boolean((window as any).__pcsx2Storage));
  return page;
}

async function listStored(page: Page): Promise<StoredEntry[]> {
  return page.evaluate(() => (window as any).__pcsx2Storage.list());
}

test.beforeAll(async ({ profile }, workerInfo) => {
  const baseURL = String(workerInfo.project.use.baseURL);
  const page = await storagePage(profile, baseURL);
  const stored = (await listStored(page)).find((entry) => entry.path === storedBios);
  const size = readFileSync(biosPath!).length;
  if (!stored || stored.size !== size) {
    await page.setInputFiles("#bios", biosPath!);
    const outcome = await page.evaluate(() => (window as any).__pcsx2StorageLastImport);
    expect(outcome, `BIOS import through storage.html: ${JSON.stringify(outcome)}`).toEqual(expect.objectContaining({ ok: true }));
    const after = (await listStored(page)).find((entry) => entry.path === storedBios);
    expect(after?.size).toBe(size);
  }
  await page.close();
});

// Imports one fixture image from the library into games/ (a complete, verified
// import is reused) and returns its mount path.
async function importImage(profile: BrowserContext, baseURL: string, image: FixtureImage): Promise<string> {
  const page = await storagePage(profile, baseURL);
  const result = await page.evaluate((name) => (window as any).__pcsx2Storage.importFromLibrary(name, "games"), image.name);
  expect(result.verified, `library import of ${image.name}: ${JSON.stringify(result)}`).toBe(true);
  expect(result.size).toBe(readFileSync(image.path).length);
  expect(result.mountedPath).toBe(`/opfs/${GAMES_DIR}/${image.name}`);
  await page.close();
  return result.mountedPath as string;
}

async function runTarget(profile: BrowserContext, baseURL: string, target: string, options: Record<string, unknown>) {
  const page = await profile.newPage();
  await page.goto(new URL("runtime.html", baseURL).href);
  await page.waitForFunction(() => Boolean((window as any).__pcsx2Runtime));
  const report = await page.evaluate(async ({ target, options }) => {
    try {
      return await (window as any).__pcsx2Runtime.run(target, options);
    } catch (error) {
      return { schema: 1, emulator: "pcsx2", ok: false, detail: error instanceof Error ? `${error.name}: ${error.message}` : String(error), frames: [], events: [], tty: [], emu: {} };
    }
  }, { target, options });
  await page.close();
  return report;
}

for (const fixture of fixtures) {
  for (const image of fixture.images) {
    test(`${fixture.name}/${image.name}: boots from origin-private storage and matches the native oracle`, async ({ profile }, testInfo) => {
      test.setTimeout(RUN_TIMEOUT_MS + 90_000);
      const baseURL = String(testInfo.project.use.baseURL);
      const mounted = await importImage(profile, baseURL, image);

      const options = {
        frames: fixture.config.kit.frames,
        render: false,
        cpu: fixture.config.kit.cpu,
        bios: storedBios,
        trace: { cpu: fixture.config.trace.cpu, ramEvery: fixture.config.trace.ramEvery, tty: fixture.config.trace.tty },
        timeoutMs: RUN_TIMEOUT_MS,
      };
      const report = await runTarget(profile, baseURL, mounted, options);
      const label = `${fixture.name}.${path.extname(image.name).slice(1)}`;
      const evidence = async (name: string, body: string, contentType: string) => {
        const file = testInfo.outputPath(name);
        writeFileSync(file, body);
        await testInfo.attach(name, { path: file, contentType });
      };
      await evidence(`${label}.report.json`, JSON.stringify(report, null, 2), "application/json");
      if (report.tty?.length) await evidence(`${label}.tty.txt`, report.tty.join("\n") + "\n", "text/plain");
      if (report.emu?.cpuJsonl) await evidence(`${label}.cpu.jsonl`, report.emu.cpuJsonl, "application/x-ndjson");

      const validation = validateReport(report);
      expect(validation.errors, "report schema").toEqual([]);

      const tty: TtyVerdict = compareTty(readFileSync(fixture.expectedTtyPath!, "utf8"), report.tty ?? [], fixture.config.ttyFilter);
      let cpu: CpuVerdict | undefined;
      if (fixture.expectedCpuPath && typeof report.emu?.cpuJsonl === "string") {
        cpu = compareCpu(readFileSync(fixture.expectedCpuPath, "utf8"), report.emu.cpuJsonl, fixture.config.kit.frames);
      }
      const summary = summarizeRun(`${fixture.name}/${image.name}`, report, { tty, cpu });
      process.stdout.write(`${summary}\nopfs handle mode: ${report.emu?.opfsHandleMode}\n`);

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
      expect(report.emu.disc).toBe(mounted);
      // Chromium grants the read-only sync access handle the reader asks for first.
      expect(report.emu.opfsHandleMode).toBe("read-only");
    });
  }
}

test("a memory card created during a run is written back to storage and restored by the next run", async ({ profile }, testInfo) => {
  test.setTimeout(RUN_TIMEOUT_MS + 90_000);
  const baseURL = String(testInfo.project.use.baseURL);
  const fixture = fixtures[0]!;
  const image = fixture.images[0]!;
  const mounted = await importImage(profile, baseURL, image);
  const storage = await storagePage(profile, baseURL);
  await storage.evaluate((target) => (window as any).__pcsx2Storage.remove(target), STORED_MEMCARD);

  const options = {
    frames: 30,
    render: false,
    cpu: fixture.config.kit.cpu,
    bios: storedBios,
    settings: { "MemoryCards/Slot1_Enable": "true", "MemoryCards/Slot1_Filename": MEMCARD },
    trace: { cpu: false, ramEvery: 0, tty: true },
    timeoutMs: RUN_TIMEOUT_MS,
  };
  const first = await runTarget(profile, baseURL, mounted, options);
  await testInfo.attach("memcard-first.report.json", { body: JSON.stringify(first, null, 2), contentType: "application/json" });
  expect(first.ok, first.detail).toBe(true);
  // Other runs in this profile may have left inis (playtime.dat) behind; only the card matters here.
  expect(first.emu.persistence.restored.some((entry: { path: string }) => entry.path.endsWith(MEMCARD)), "no memory card to restore on the first run").toBe(false);
  const written = first.emu.persistence.saved.find((entry: { path: string }) => entry.path === STORED_MEMCARD);
  expect(written, `memory card written back: ${JSON.stringify(first.emu.persistence)}`).toBeDefined();
  const stored = (await listStored(storage)).find((entry) => entry.path === STORED_MEMCARD);
  expect(stored?.size).toBe(written.bytes);
  expect(stored?.size).toBeGreaterThan(8 * 1024 * 1024);

  const second = await runTarget(profile, baseURL, mounted, options);
  await testInfo.attach("memcard-second.report.json", { body: JSON.stringify(second, null, 2), contentType: "application/json" });
  expect(second.ok, second.detail).toBe(true);
  expect(second.emu.persistence.restored).toContainEqual({ path: `/pcsx2/memcards/${MEMCARD}`, bytes: written.bytes });
  process.stdout.write(`memory card persistence: first run saved ${JSON.stringify(first.emu.persistence.saved)}, second run restored ${JSON.stringify(second.emu.persistence.restored)}\n`);

  const removed = await storage.evaluate((target) => (window as any).__pcsx2Storage.remove(target), STORED_MEMCARD);
  expect(removed.removed).toBe(true);
  await storage.close();
});

export type { Fixture };
