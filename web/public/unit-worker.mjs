// Runs the gtest module (pcsx2-web-units.mjs, web/host/pcsx2_web_unit_main.cpp)
// and hands back the JSON report it prints after PCSX2_WEB_UNIT_REPORT=.
self.addEventListener("message", async (event) => {
  if (event.data?.type !== "run-units") return;

  const output = [];
  try {
    const coreUrl = event.data.coreUrl ? new URL(event.data.coreUrl, self.location.href).href : new URL("./core/pcsx2-web-units.mjs", self.location.href).href;
    const { default: createPCSX2Units } = await import(coreUrl);
    const module = await createPCSX2Units({
      locateFile: (name) => new URL(name, coreUrl).href,
      pthreadPoolSize: event.data.pthreadPoolSize ?? 4,
      noInitialRun: true,
      print: (line) => output.push(String(line)),
      printErr: (line) => output.push(String(line)),
    });
    let exitCode = -1;
    try {
      exitCode = module.callMain([]);
    } catch (error) {
      output.push(`callMain threw: ${error instanceof Error ? `${error.name}: ${error.message}` : String(error)}`);
    }

    const marker = "PCSX2_WEB_UNIT_REPORT=";
    const reportLine = output.find((line) => line.startsWith(marker));
    if (!reportLine) throw new Error(`unit report marker missing (exit ${exitCode}); output: ${output.slice(-40).join("\n")}`);
    const report = JSON.parse(reportLine.slice(marker.length));
    self.postMessage({ type: "unit-result", ok: report.failed === 0 && exitCode === 0, exitCode, report, output });
  } catch (error) {
    self.postMessage({
      type: "unit-result",
      ok: false,
      error: error instanceof Error ? `${error.name}: ${error.message}\n${error.stack || ""}` : String(error),
      output,
    });
  }
});
