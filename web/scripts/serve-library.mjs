// Same-origin library for storage imports: every fixture ELF under
// tests/fixtures/<name>/, served by the kit's library server (Range, HEAD,
// ETag, index.json with SHA-256). The preview server proxies /library/ here.
//
//   node scripts/serve-library.mjs [--port 4195] [--host 0.0.0.0] [--dir <fixtures dir>]
import { readdir } from "node:fs/promises";
import path from "node:path";
import { fileURLToPath } from "node:url";
import { createLibraryServer } from "@appmana-public/disc-images/serve-library";

const root = path.resolve(path.dirname(fileURLToPath(import.meta.url)), "..");
let port = 4195;
let host = "0.0.0.0";
let fixturesDir = path.join(root, "tests", "fixtures");
const argv = process.argv.slice(2);
for (let i = 0; i < argv.length; i++) {
  const value = argv[i + 1];
  switch (argv[i]) {
    case "--port": port = Number(value); i++; break;
    case "--host": host = String(value); i++; break;
    case "--dir": fixturesDir = path.resolve(String(value)); i++; break;
    default: break;
  }
}

const files = [];
for (const entry of (await readdir(fixturesDir, { withFileTypes: true })).sort((a, b) => a.name.localeCompare(b.name))) {
  if (!entry.isDirectory()) continue;
  for (const name of (await readdir(path.join(fixturesDir, entry.name))).sort()) {
    if (name.endsWith(".elf")) files.push(path.join(fixturesDir, entry.name, name));
  }
}
if (!files.length) throw new Error(`no fixture ELFs under ${fixturesDir}`);

const library = await createLibraryServer({ files, log: (line) => process.stderr.write(`${line}\n`) });
const address = await library.listen(port, host);
process.stderr.write(`library: listening on http://${address.address}:${address.port}${library.prefix}/ with ${library.entries.size} files\n`);
for (const entry of library.entries.values()) process.stderr.write(`library:   ${entry.name} (${entry.size} bytes)${entry.sha256 ? "" : " hashing"}\n`);
const shutdown = () => { library.close().then(() => process.exit(0)); };
process.on("SIGINT", shutdown);
process.on("SIGTERM", shutdown);
