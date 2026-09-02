// Which Chrome launch recipe yields a hardware WebGPU adapter on this box,
// headless and headed. Prints one line per configuration.
//   DISPLAY=:0 XAUTHORITY=... NODE_PATH=<dir with playwright> node web/spikes/chrome-adapter-matrix.mjs
import { createRequire } from "node:module";
import { mkdtemp } from "node:fs/promises";
import { tmpdir } from "node:os";
import path from "node:path";
const { chromium } = createRequire(import.meta.url)("playwright");
const base = ["--no-sandbox", "--enable-unsafe-webgpu", "--enable-webgpu-developer-features", "--ignore-gpu-blocklist"];
const recipes = {
  "vulkan-angle": [...base, "--enable-features=Vulkan", "--use-angle=vulkan"],
  "angle-vulkan-from-angle": [...base, "--use-gl=angle", "--use-angle=vulkan", "--enable-features=Vulkan,VulkanFromANGLE,DefaultANGLEVulkan"],
  "egl-gl": [...base, "--use-gl=egl"],
  "plain": [...base],
};
for (const headless of [true, false]) {
  for (const [name, args] of Object.entries(recipes)) {
    const profile = await mkdtemp(path.join(tmpdir(), "chrome-adapter-"));
    let line = `headless=${headless} ${name}: `;
    try {
      const ctx = await chromium.launchPersistentContext(profile, { executablePath: "/usr/bin/google-chrome", headless, args, timeout: 30000 });
      const page = await ctx.newPage();
      await page.route("http://localhost/spike", (r) => r.fulfill({ contentType: "text/html", body: "<title>s</title>" }));
      await page.goto("http://localhost/spike");
      const r = await page.evaluate(async () => {
        const out = [];
        for (const pref of ["high-performance", undefined]) {
          const a = await navigator.gpu?.requestAdapter(pref ? { powerPreference: pref } : undefined);
          out.push(a ? `${a.info?.vendor}/${a.info?.architecture}/${a.info?.device} fallback=${a.info?.isFallbackAdapter} features=${a.features.size}` : "null");
        }
        return out.join(" | ");
      });
      line += r;
      await ctx.close();
    } catch (e) { line += `ERROR ${String(e).split("\n")[0]}`; }
    console.log(line);
  }
}
