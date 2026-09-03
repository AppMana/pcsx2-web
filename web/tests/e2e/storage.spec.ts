import { createHash } from "node:crypto";
import { readFileSync } from "node:fs";
import { expect, test } from "@playwright/test";

type StorageApi = {
  list: (options?: Record<string, unknown>) => Promise<Array<{ path: string; size?: number; import?: { complete: boolean } }>>;
  remove: (path: string, options?: Record<string, unknown>) => Promise<{ removed: boolean }>;
  importBios: (files: File[], options?: Record<string, unknown>) => Promise<{ files: string[]; bytes: number; destination: string }>;
  libraryIndex: () => Promise<{ files: Array<{ name: string; size: number; sha256: string | null }> }>;
  importFromLibrary: (name: string, destination?: string, options?: Record<string, unknown>) => Promise<any>;
  mountPath: (path: string) => string;
};

const fixtureSha256 = (relative: string) => createHash("sha256").update(readFileSync(relative)).digest("hex");

test("imports a BIOS into persistent storage under pcsx2/bios and retains it across reload", async ({ page }) => {
  await page.goto("/storage.html");
  const first = await page.evaluate(async () => {
    const api = (window as typeof window & { __pcsx2Storage: StorageApi }).__pcsx2Storage;
    const payload = new File([new Uint8Array([0x50, 0x53, 0x32, 0x00, 0x01])], "opfs-test.bin");
    const result = await api.importBios([payload]);
    return { result, files: await api.list(), mounted: api.mountPath(result.files[0]!) };
  });
  expect(first.result.destination).toBe("pcsx2/bios");
  expect(first.result.files).toEqual(["pcsx2/bios/opfs-test.bin"]);
  expect(first.result.bytes).toBe(5);
  expect(first.mounted).toBe("/opfs/pcsx2/bios/opfs-test.bin");
  expect(first.files).toContainEqual(expect.objectContaining({ path: "pcsx2/bios/opfs-test.bin", size: 5 }));
  await expect(page.locator("#files")).toContainText("pcsx2/bios/opfs-test.bin");

  await page.reload();
  const files = await page.evaluate(() => (window as typeof window & { __pcsx2Storage: StorageApi }).__pcsx2Storage.list());
  expect(files).toContainEqual(expect.objectContaining({ path: "pcsx2/bios/opfs-test.bin", size: 5 }));

  const removed = await page.evaluate(() => (window as typeof window & { __pcsx2Storage: StorageApi }).__pcsx2Storage.remove("pcsx2/bios/opfs-test.bin"));
  expect(removed.removed).toBe(true);
  const after = await page.evaluate(() => (window as typeof window & { __pcsx2Storage: StorageApi }).__pcsx2Storage.list());
  expect(after.some((entry) => entry.path === "pcsx2/bios/opfs-test.bin")).toBe(false);
});

test("imports a fixture ELF from the same-origin library with range requests and verifies its SHA-256", async ({ page }) => {
  await page.goto("/storage.html");
  const index = await page.evaluate(() => (window as typeof window & { __pcsx2Storage: StorageApi }).__pcsx2Storage.libraryIndex());
  const entry = index.files.find((file) => file.name === "hello_tty.elf");
  expect(entry?.sha256).toBe(fixtureSha256("tests/fixtures/hello_tty/hello_tty.elf"));

  const result = await page.evaluate(() => (window as typeof window & { __pcsx2Storage: StorageApi }).__pcsx2Storage.importFromLibrary("hello_tty.elf", "games", { chunkSize: 65536, restart: true }));
  expect(result.verified).toBe(true);
  expect(result.sha256).toBe(fixtureSha256("tests/fixtures/hello_tty/hello_tty.elf"));
  expect(result.size).toBe(entry!.size);
  expect(result.sessionBytes).toBe(entry!.size);
  expect(result.requests).toBe(Math.ceil(entry!.size / 65536));
  expect(result.mountedPath).toBe("/opfs/games/hello_tty.elf");

  const files = await page.evaluate(() => (window as typeof window & { __pcsx2Storage: StorageApi }).__pcsx2Storage.list());
  expect(files).toContainEqual(expect.objectContaining({ path: "games/hello_tty.elf", size: entry!.size, import: expect.objectContaining({ complete: true }) }));

  const again = await page.evaluate(() => (window as typeof window & { __pcsx2Storage: StorageApi }).__pcsx2Storage.importFromLibrary("hello_tty.elf", "games"));
  expect(again.alreadyComplete).toBe(true);
  expect(again.sessionBytes).toBe(0);
  expect(again.verified).toBe(true);
});
