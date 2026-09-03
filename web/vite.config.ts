import { createReadStream } from "node:fs";
import { stat } from "node:fs/promises";
import type { IncomingMessage, ServerResponse } from "node:http";
import path from "node:path";
import { defineConfig, type Plugin } from "vitest/config";
import { probeWebGpuCapabilities } from "@appmana-public/web-emulator-harness/playwright";

const isolationHeaders = {
  "Cross-Origin-Opener-Policy": "same-origin",
  "Cross-Origin-Embedder-Policy": "require-corp",
  "Cross-Origin-Resource-Policy": "same-origin",
};

// The same-origin file library (scripts/serve-library.mjs) that OPFS imports
// download from. The hosted Ingress routes /library/ to it directly; locally
// the dev and preview servers proxy the prefix so the page stays same-origin.
const libraryProxy = {
  "^/library/": { target: process.env.PCSX2_LIBRARY_PROXY || "http://127.0.0.1:4195", changeOrigin: false },
};

const FIXTURE_TYPES: Record<string, string> = {
  ".elf": "application/octet-stream",
  ".iso": "application/octet-stream",
  ".chd": "application/octet-stream",
  ".toml": "text/plain; charset=utf-8",
  ".txt": "text/plain; charset=utf-8",
  ".md": "text/markdown; charset=utf-8",
  ".jsonl": "application/x-ndjson",
  ".json": "application/json",
  ".png": "image/png",
};

// Serves the committed fixture tree (tests/fixtures/<name>/<name>.elf and its
// expected/ outputs) at /tests/fixtures/ from both the dev and the preview
// server, so runtime.html boots fixtures by their repository path without a
// second copy under public/.
function serveFixtures(): Plugin {
  const root = path.resolve("tests/fixtures");
  const prefix = "/tests/fixtures/";
  const middleware = async (request: IncomingMessage, response: ServerResponse, next: () => void) => {
    const pathname = new URL(request.url ?? "/", "http://localhost").pathname;
    if (!pathname.startsWith(prefix)) return next();
    const relative = path.posix.normalize(decodeURIComponent(pathname.slice(prefix.length)));
    if (relative.startsWith("../") || relative === "..") {
      response.statusCode = 403;
      response.end();
      return;
    }
    const file = path.join(root, relative);
    let size: number;
    try {
      const info = await stat(file);
      if (!info.isFile()) return next();
      size = info.size;
    } catch {
      response.statusCode = 404;
      response.end();
      return;
    }
    response.setHeader("Content-Type", FIXTURE_TYPES[path.extname(file)] ?? "application/octet-stream");
    response.setHeader("Content-Length", size);
    response.setHeader("Cache-Control", "no-store");
    response.setHeader("Cross-Origin-Resource-Policy", "same-origin");
    if (request.method === "HEAD") {
      response.end();
      return;
    }
    createReadStream(file).pipe(response);
  };
  return {
    name: "pcsx2-fixtures",
    configureServer(server) { server.middlewares.use(middleware); },
    configurePreviewServer(server) { server.middlewares.use(middleware); },
  };
}

// The kit's probeWebGpuCapabilities is written to be evaluated inside a page
// (Playwright passes it to page.evaluate). The dashboard runs the same
// function; its source is inlined here so the browser bundle never pulls the
// kit's Playwright imports.
const PROBE_ID = "virtual:web-emulator-kit/webgpu-probe";
function kitWebGpuProbe(): Plugin {
  return {
    name: "pcsx2-kit-webgpu-probe",
    resolveId(id) { return id === PROBE_ID ? `\0${PROBE_ID}` : undefined; },
    load(id) {
      if (id !== `\0${PROBE_ID}`) return undefined;
      return `export const probeWebGpuCapabilities = (${probeWebGpuCapabilities.toString()});\n`;
    },
  };
}

export default defineConfig({
  base: process.env.BASE_PATH || "/",
  plugins: [serveFixtures(), kitWebGpuProbe()],
  server: { headers: isolationHeaders, proxy: libraryProxy },
  preview: {
    headers: isolationHeaders,
    proxy: libraryProxy,
    // The stable HTTPS origin for devices is an nginx Ingress that proxies to
    // this preview server, so its host header must be accepted.
    allowedHosts: ["pcsx2.appmana.com"],
  },
  build: {
    target: "safari26",
    sourcemap: true,
  },
  test: {
    include: ["tests/*.test.ts"],
  },
});
