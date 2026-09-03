import { expect, test } from "@playwright/test";

// The gtest count pinned for pcsx2_web_unit_tests (tests/ctest/common plus
// the core patch and swizzle suites); a build that drops or adds a test must
// update this number deliberately.
const UNIT_TEST_COUNT = 102;

test("PCSX2's own unit tests pass in wasm", async ({ page }, testInfo) => {
  test.setTimeout(180_000);
  await page.goto("/units.html");
  const result = await page.waitForFunction(
    () => (globalThis as typeof globalThis & { __pcsx2UnitResult?: unknown }).__pcsx2UnitResult,
    undefined,
    { timeout: 150_000 },
  ).then((handle) => handle.jsonValue() as Promise<{
    ok: boolean;
    exitCode?: number;
    error?: string;
    output?: string[];
    report?: { target: string; total: number; passed: number; failed: number; skipped: number; tests: Array<{ suite: string; name: string; status: string; failures: unknown[] }> };
  }>);
  await testInfo.attach("units.json", { body: JSON.stringify(result, null, 2), contentType: "application/json" });

  expect(result.error).toBeUndefined();
  const failed = result.report?.tests.filter((entry) => entry.status === "failed") ?? [];
  expect(failed, JSON.stringify(failed, null, 2)).toEqual([]);
  expect(result.ok).toBe(true);
  expect(result.exitCode).toBe(0);
  expect(result.report?.target).toBe("wasm32-emscripten");
  expect(result.report?.total).toBe(UNIT_TEST_COUNT);
  expect(result.report?.passed).toBe(UNIT_TEST_COUNT);
  expect(result.report?.failed).toBe(0);
  expect(result.report?.skipped).toBe(0);
  expect(result.report?.tests).toHaveLength(UNIT_TEST_COUNT);
});
