// Headless Chromium smoke run for the wasm targets: runs the unit test module and
// checks that the runtime module instantiates and pcsx2_web_init() returns 0.
// usage: node web/scripts/wasm-smoke.mjs <dir-with-mjs-and-wasm> [units|runtime|all]
import http from 'node:http';
import path from 'node:path';
import fs from 'node:fs';
import { createRequire } from 'node:module';

const require = createRequire(process.env.PLAYWRIGHT_ROOT ? path.join(process.env.PLAYWRIGHT_ROOT, 'package.json') : import.meta.url);
const { chromium } = require('playwright');

const dir = path.resolve(process.argv[2]);
const mode = process.argv[3] || 'all';
const types = { '.mjs': 'text/javascript', '.js': 'text/javascript', '.wasm': 'application/wasm', '.html': 'text/html' };

const pages = {
  'units.html': `<!doctype html><script type="module">
    const out = [];
    const m = await (await import('./pcsx2-web-units.mjs')).default({ print: s => out.push(s), printErr: s => out.push(s), pthreadPoolSize: 4 });
    let code = -1;
    try { code = m.callMain([]); } catch (e) { out.push('callMain threw: ' + e); }
    window.__result = { code, out };
  </script>`,
  'runtime.html': `<!doctype html><script type="module">
    const out = [];
    window.__out = out;
    window.__stage = 'import';
    const t0 = performance.now();
    const m = await (await import('./pcsx2-web.mjs')).default({ print: s => out.push(s), printErr: s => out.push(s), pthreadPoolSize: 8 });
    window.__stage = 'instantiated';
    const tInst = performance.now() - t0;
    const rc = m._pcsx2_web_init();
    window.__stage = 'init returned ' + rc;
    const workers = () => Object.keys(m.PThread.pthreads).length;
    const poll = [];
    for (let i = 0; i < 100; i++) {
      await new Promise(r => setTimeout(r, 100));
      const st = m._pcsx2_web_status();
      poll.push([st, workers()]);
      if (st !== 1) break;
    }
    const n = m._pcsx2_web_tty_pending();
    const buf = m._malloc(n + 1);
    const got = m._pcsx2_web_tty_read(buf, n);
    const tty = m.UTF8ToString(buf, got);
    m._free(buf);
    window.__result = { rc, tInst, status: m._pcsx2_web_status(), runningWorkers: workers(), unusedWorkers: m.PThread.unusedWorkers.length, poll, tty, out, heap: m.HEAPU8.length };
  </script>`,
};

const server = http.createServer((req, res) => {
  const name = decodeURIComponent(req.url.split('?')[0].slice(1));
  res.setHeader('Cross-Origin-Opener-Policy', 'same-origin');
  res.setHeader('Cross-Origin-Embedder-Policy', 'require-corp');
  if (pages[name]) { res.setHeader('Content-Type', 'text/html'); res.end(pages[name]); return; }
  const file = path.join(dir, name);
  if (!fs.existsSync(file)) { res.statusCode = 404; res.end(); return; }
  res.setHeader('Content-Type', types[path.extname(file)] || 'application/octet-stream');
  fs.createReadStream(file).pipe(res);
});
await new Promise(r => server.listen(0, '127.0.0.1', r));
const base = `http://127.0.0.1:${server.address().port}/`;

const browser = await chromium.launch({ headless: true });
let failed = false;
for (const which of mode === 'all' ? ['units', 'runtime'] : [mode]) {
  const page = await browser.newPage();
  const console_lines = [];
  page.on('console', msg => console_lines.push(msg.text()));
  page.on('pageerror', err => console_lines.push('pageerror: ' + err.message));
  await page.goto(base + which + '.html');
  try {
    await page.waitForFunction(() => window.__result !== undefined, null, { timeout: 180000 });
  } catch (e) {
    const stage = await page.evaluate(() => window.__stage);
    const out = await page.evaluate(() => (window.__out || []).slice(-40));
    console.log(`${which}: TIMEOUT at stage '${stage}'\n  module output:\n  ` + out.join('\n  ') + '\n  console:\n  ' + console_lines.slice(-40).join('\n  '));
    failed = true;
    await page.close();
    continue;
  }
  const result = await page.evaluate(() => window.__result);
  if (which === 'units') {
    const report = result.out.find(l => l.startsWith('PCSX2_WEB_UNIT_REPORT=')) || '';
    const json = report ? JSON.parse(report.slice('PCSX2_WEB_UNIT_REPORT='.length)) : null;
    console.log(`units: exit=${result.code} total=${json?.total} passed=${json?.passed} failed=${json?.failed} skipped=${json?.skipped}`);
    for (const t of json?.tests || []) if (t.status === 'failed') console.log('  FAILED', t.suite + '.' + t.name, JSON.stringify(t.failures));
    if (!json || result.code !== 0 || json.failed !== 0) { failed = true; console.log(result.out.slice(-30).join('\n')); }
  } else {
    console.log(`runtime: init=${result.rc} status=${result.status} runningWorkers=${result.runningWorkers} unusedWorkers=${result.unusedWorkers} heap=${result.heap} instantiateMs=${result.tInst.toFixed(0)}`);
    console.log('  poll(status,workers):', JSON.stringify(result.poll));
    console.log('  tty tail:\n' + result.tty.split('\n').slice(-25).join('\n'));
    if (result.rc !== 0 || result.status !== 0) { failed = true; console.log(result.out.slice(-30).join('\n')); }
  }
  const errs = console_lines.filter(l => /error|abort|unreachable|RuntimeError/i.test(l));
  if (errs.length) console.log('  browser console errors:\n  ' + errs.slice(0, 20).join('\n  '));
  await page.close();
}
await browser.close();
server.close();
process.exit(failed ? 1 : 0);
