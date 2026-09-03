// The BIOS a spec boots with comes from PCSX2_BIOS (a path to the .bin) and
// is imported once into the persistent browser profile's origin-private
// storage through storage.html, the same path a user takes.
import { readFileSync } from "node:fs";
import os from "node:os";
import path from "node:path";
import { expect, type BrowserContext } from "@playwright/test";

export const biosPath = process.env.PCSX2_BIOS;
export const profileDir = process.env.PCSX2_CHROME_PROFILE ?? path.join(os.homedir(), ".cache", "pcsx2-web-playwright-profile");
export const biosName = biosPath ? path.basename(biosPath) : "";
export const storedBios = `pcsx2/bios/${biosName}`;

export async function ensureStoredBios(profile: BrowserContext, baseURL: string): Promise<void> {
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
}
