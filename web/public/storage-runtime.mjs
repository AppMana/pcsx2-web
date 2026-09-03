// Storage page on @appmana-public/disc-images (staged same-origin under
// ./kit/disc-images by scripts/stage-assets.mjs). PCSX2 rules live here: the
// BIOS goes to pcsx2/bios/, games to games/, the mount root is /opfs.
import {
  abortImport,
  fetchIndex,
  formatBytes,
  formatRate,
  importFile,
  importFiles,
  importFromURL,
  importProgress,
  list,
  mountPath,
  remove,
  requestPersistentStorage,
  resolveIndexEntry,
  storageStatus,
} from "./kit/disc-images/browser/index.js";

export const MOUNT_ROOT = "/opfs";
export const BIOS_DIR = "pcsx2/bios";
export const GAMES_DIR = "games";
const LIBRARY_BASE = new URL("./library/", location.href);

const capacity = document.querySelector("#capacity");
const progress = document.querySelector("#progress");
const transfer = document.querySelector("#transfer");
const filesElement = document.querySelector("#files");
const libraryFile = document.querySelector("#library-file");
const libraryDestination = document.querySelector("#library-destination");
const libraryImportButton = document.querySelector("#library-import");
const libraryAbortButton = document.querySelector("#library-abort");
const libraryStatus = document.querySelector("#library-status");

async function updateCapacity() {
  const state = await storageStatus();
  const free = Math.max(0, state.quota - state.usage);
  capacity.className = state.supported ? "good" : "warn";
  capacity.textContent = state.supported
    ? `${formatBytes(state.usage)} used · ${formatBytes(free)} available · ${formatBytes(state.quota)} quota · ${state.persisted ? "persistent" : "best-effort"}`
    : "This browser does not expose origin-private file storage.";
  return state;
}

async function refreshFiles() {
  const entries = await list();
  filesElement.textContent = entries.length
    ? entries.map((entry) => `${(entry.locked ? "(importing)" : formatBytes(entry.size ?? 0)).padStart(11)}  ${entry.path}${entry.import && !entry.import.complete ? "  (partial import)" : ""}`).join("\n")
    : "None";
  return entries;
}

function report(message) {
  const total = message.total ?? message.size ?? 0;
  const written = message.written ?? message.offset ?? 0;
  progress.max = Math.max(1, total);
  progress.value = written;
  transfer.textContent = `${message.path} · ${formatBytes(written)} / ${formatBytes(total)}`;
}

let lastImport;
async function runImport(action) {
  progress.value = 0;
  transfer.className = "";
  lastImport = (async () => {
    try {
      const result = await action();
      transfer.className = "good";
      transfer.textContent = `Stored ${formatBytes(result.bytes ?? result.size ?? 0)} at ${result.mountedPath ?? result.files?.map((file) => mountPath(file, MOUNT_ROOT)).join(", ") ?? ""}`;
      await Promise.all([updateCapacity(), refreshFiles()]);
      return result;
    } catch (error) {
      transfer.className = "warn";
      transfer.textContent = error instanceof Error ? error.message : String(error);
      throw error;
    }
  })();
  window.__pcsx2StorageLastImport = lastImport.then((result) => ({ ok: true, result }), (error) => ({ ok: false, name: error?.name, message: error?.message, report: error?.report }));
  return lastImport;
}

// BIOS images keep their file names under pcsx2/bios/ so PCSX2's BIOS
// scanner (Folders/Bios) finds them by extension.
export function importBios(files, options = {}) {
  return importFiles(files, { ...options, destination: BIOS_DIR, mountRoot: MOUNT_ROOT });
}

export function importGames(files, options = {}) {
  return importFiles(files, { ...options, destination: GAMES_DIR, mountRoot: MOUNT_ROOT });
}

function describeProgress(message) {
  const percent = message.total ? ((message.offset / message.total) * 100).toFixed(1) : "0.0";
  const phase = message.phase === "verifying-local" ? "verifying stored bytes" : message.phase === "waiting-for-hash" ? "waiting for server hash" : "downloading";
  const rate = message.instantRateBytesPerSecond ? ` · now ${formatRate(message.instantRateBytesPerSecond)}` : "";
  return `${phase} · ${formatBytes(message.offset ?? 0)} / ${formatBytes(message.total ?? 0)} · ${percent}% · avg ${formatRate(message.rateBytesPerSecond ?? 0)}${rate} · ${message.requests ?? 0} requests`;
}

export async function importFromLibrary(name, destination = GAMES_DIR, options = {}) {
  const entry = await resolveIndexEntry(LIBRARY_BASE, name, { onWaiting: () => { libraryStatus.textContent = `waiting for the library to hash ${name}`; } });
  const { onProgress, ...request } = options;
  return importFromURL(entry.url, `${destination}/${name}`, {
    ...request,
    size: entry.size,
    sha256: entry.sha256,
    etag: entry.etag ?? undefined,
    mountRoot: MOUNT_ROOT,
    onProgress: (message) => {
      progress.max = Math.max(1, message.total ?? 1);
      progress.value = message.offset ?? 0;
      transfer.textContent = `${destination}/${name} · ${describeProgress(message)}`;
      libraryStatus.textContent = describeProgress(message);
      onProgress?.(message);
    },
  });
}

async function runLibraryImport(name, destination, options = {}) {
  libraryImportButton.disabled = true;
  libraryAbortButton.disabled = false;
  libraryStatus.className = "";
  libraryStatus.textContent = `Preparing ${name}…`;
  try {
    const result = await runImport(() => importFromLibrary(name, destination, options));
    libraryStatus.className = result.verified ? "good" : "warn";
    libraryStatus.textContent = result.alreadyComplete
      ? `${result.path} was already imported and verified (${formatBytes(result.size)})`
      : `${result.path} · ${formatBytes(result.sessionBytes)} downloaded in ${(result.elapsedMs / 1000).toFixed(1)} s (${formatRate(result.rateBytesPerSecond)}) · resumed from ${formatBytes(result.resumedFrom)} · ${result.requests} range requests · SHA-256 ${result.verified ? "verified" : "MISMATCH"}`;
    return result;
  } catch (error) {
    libraryStatus.className = "warn";
    const partial = error?.report;
    libraryStatus.textContent = `${error?.name === "AbortError" ? "Aborted" : "Failed"}: ${error?.message}${partial?.offset !== undefined ? ` · ${formatBytes(partial.offset)} stored; run again to resume` : ""}`;
    throw error;
  } finally {
    libraryImportButton.disabled = false;
    libraryAbortButton.disabled = true;
  }
}

async function refreshLibrary() {
  try {
    const index = await fetchIndex(LIBRARY_BASE);
    libraryFile.replaceChildren(...(index.files ?? []).map((entry) => {
      const option = document.createElement("option");
      option.value = entry.name;
      option.textContent = `${entry.name} · ${formatBytes(entry.size)}${entry.sha256 ? "" : " · hashing…"}`;
      return option;
    }));
    if (!index.files?.length) libraryFile.replaceChildren(new Option("Library is empty", ""));
    return index;
  } catch (error) {
    libraryFile.replaceChildren(new Option(`Library unavailable: ${error.message}`, ""));
    return undefined;
  }
}

document.querySelector("#persist").addEventListener("click", async () => {
  const state = await requestPersistentStorage();
  await updateCapacity();
  transfer.textContent = state.persisted
    ? "The browser granted persistent storage."
    : "Storage remains best-effort; keep ample free device space and retain the source files.";
});
document.querySelector("#bios").addEventListener("change", (event) => {
  if (event.target.files.length) void runImport(() => importBios(event.target.files, { onProgress: report })).catch(() => {});
});
document.querySelector("#game").addEventListener("change", (event) => {
  if (event.target.files.length) void runImport(() => importGames(event.target.files, { onProgress: report })).catch(() => {});
});
document.querySelector("#refresh").addEventListener("click", refreshFiles);
libraryImportButton.addEventListener("click", () => {
  if (libraryFile.value) void runLibraryImport(libraryFile.value, libraryDestination.value).catch(() => {});
});
libraryAbortButton.addEventListener("click", () => abortImport());

window.__pcsx2Storage = {
  mountRoot: MOUNT_ROOT,
  biosDir: BIOS_DIR,
  gamesDir: GAMES_DIR,
  status: storageStatus,
  persist: requestPersistentStorage,
  list: (options) => list(options),
  remove: (path, options) => remove(path, options).then(async (result) => { await Promise.all([updateCapacity(), refreshFiles()]); return result; }),
  importBios: (files, options) => runImport(() => importBios(files, options)),
  importGames: (files, options) => runImport(() => importGames(files, options)),
  importFile: (file, path, options) => runImport(() => importFile(file, path, { ...options, mountRoot: MOUNT_ROOT })),
  libraryIndex: () => fetchIndex(LIBRARY_BASE),
  importFromLibrary: (name, destination, options) => runLibraryImport(name, destination ?? GAMES_DIR, options),
  importProgress,
  abortImport,
  mountPath: (path) => mountPath(path, MOUNT_ROOT),
};

await Promise.all([updateCapacity(), refreshFiles(), refreshLibrary()]);

const parameters = new URLSearchParams(location.search);
const autoImport = parameters.get("import");
if (autoImport) {
  const destination = parameters.get("destination") || (/\.bin$/i.test(autoImport) ? BIOS_DIR : GAMES_DIR);
  libraryFile.value = autoImport;
  libraryDestination.value = destination;
  window.__pcsx2AutoImport = runLibraryImport(autoImport, destination, {
    restart: parameters.get("restart") === "1",
    verify: parameters.get("verify") === "1",
    chunkSize: parameters.get("chunk") ? Number(parameters.get("chunk")) : undefined,
  }).catch((error) => ({ failed: true, error: error.message }));
}
