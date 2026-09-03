// Copies into public/ what the plain-ESM pages need same-origin: the
// COOP/COEP service worker and the kit's browser storage modules (the
// import worker must be loaded by URL from this origin, and it imports the
// kit's core/ modules relatively).
import { cp, mkdir } from "node:fs/promises";
import { createRequire } from "node:module";
import path from "node:path";
import { fileURLToPath } from "node:url";
import { stageAssets } from "@appmana-public/web-emulator-harness/stage-assets";

const root = path.resolve(path.dirname(fileURLToPath(import.meta.url)), "..");
const output = path.join(root, "public");
const require = createRequire(import.meta.url);
const discImages = path.dirname(require.resolve("@appmana-public/disc-images/package.json"));

const { copied } = await stageAssets({ outputDir: output });
const kitOutput = path.join(output, "kit", "disc-images");
await mkdir(kitOutput, { recursive: true });
for (const directory of ["browser", "core"]) {
  await cp(path.join(discImages, directory), path.join(kitOutput, directory), { recursive: true, force: true });
  copied.push({ from: path.join(discImages, directory), to: path.join(kitOutput, directory) });
}
for (const entry of copied) process.stdout.write(`staged ${path.relative(root, entry.to)}\n`);
