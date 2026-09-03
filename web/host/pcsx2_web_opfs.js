// Origin-private file storage for the CDVD readers (pcsx2/CDVD/OpfsFileReader.cpp).
//
// Every function here runs on the OPFS pthread, a dedicated worker that sits in
// its event loop, so navigator.storage and FileSystemSyncAccessHandle are
// available and the asynchronous open can complete. The open, lock and read
// semantics are a port of @appmana-public/disc-images browser/sector-device.js
// (openReadHandle, SectorDevice.acquire/release/read) and core/paths.js
// (normalizeRelativePath): one exclusive Web Lock named "disc-images:<path>"
// per open file, Chrome's read-only handle mode first with the option-less
// call as the fallback, and reads that loop on short counts straight into the
// caller's memory. The module is not imported here because the pthread worker
// is the wasm module itself, loaded from /core/, and must not depend on where
// the page stages the kit.
addToLibrary({
  $PcsxOpfs__deps: ['$UTF8ToString', '$stringToUTF8'],
  $PcsxOpfs: {
    LOCK_PREFIX: "disc-images:",
    files: new Map(),
    nextHandle: 1,
    // In-context fallback when Web Locks are unavailable.
    openPaths: new Set(),

    normalizeRelativePath(value) {
      if (typeof value !== "string" || value.includes("\0")) throw new TypeError("Invalid storage path");
      var normalized = value.replaceAll("\\", "/").replace(/^\.\//, "");
      if (!normalized || normalized.startsWith("/")) throw new TypeError("Storage paths must be relative");
      var parts = normalized.split("/").filter((part) => part && part !== ".");
      if (!parts.length || parts.some((part) => part === "..")) throw new TypeError("Storage path escapes the storage root");
      return parts.join("/");
    },

    // Resolves with the lock's release function; rejects when another context holds it.
    async acquire(path) {
      var name = PcsxOpfs.LOCK_PREFIX + path;
      var locks = navigator.locks;
      if (!locks) {
        if (PcsxOpfs.openPaths.has(name)) throw new Error(path + " is already open in this context");
        PcsxOpfs.openPaths.add(name);
        return () => PcsxOpfs.openPaths.delete(name);
      }
      return new Promise((resolve, reject) => {
        locks.request(name, { mode: "exclusive", ifAvailable: true }, (lock) => {
          if (!lock) {
            reject(new Error(path + " is already open (lock " + name + " held elsewhere)"));
            return Promise.resolve();
          }
          return new Promise((release) => resolve(() => release(undefined)));
        }).catch(reject);
      });
    },

    // Chrome accepts { mode: "read-only" } (several readers, no write lock); browsers that
    // know the option but not the value throw a TypeError, and Safari ignores the dictionary
    // and hands out its one exclusive handle.
    async openReadHandle(fileHandle) {
      try {
        var access = await fileHandle.createSyncAccessHandle({ mode: "read-only" });
        return { access, mode: access.mode === "read-only" ? "read-only" : "readwrite" };
      } catch (error) {
        if (!(error instanceof TypeError)) throw error;
      }
      return { access: await fileHandle.createSyncAccessHandle(), mode: "readwrite" };
    },

    async open(relativePath) {
      var path = PcsxOpfs.normalizeRelativePath(relativePath);
      var release = await PcsxOpfs.acquire(path);
      try {
        if (!navigator.storage || !navigator.storage.getDirectory) throw new Error("origin-private file storage is unavailable in this worker");
        var parts = path.split("/");
        var name = parts.pop();
        var directory = await navigator.storage.getDirectory();
        for (var part of parts) directory = await directory.getDirectoryHandle(part);
        var fileHandle = await directory.getFileHandle(name);
        var opened = await PcsxOpfs.openReadHandle(fileHandle);
        var handle = PcsxOpfs.nextHandle++;
        PcsxOpfs.files.set(handle, { path, access: opened.access, mode: opened.mode, release });
        return { handle, size: opened.access.getSize(), mode: opened.mode };
      } catch (error) {
        release();
        throw error;
      }
    },
  },

  // Writes an OpfsOpenResult (pcsx2/CDVD/OpfsFileReader.cpp) at resultPtr and finishes ctx.
  pcsx2_web_opfs_open__deps: ['$PcsxOpfs', 'emscripten_proxy_finish'],
  pcsx2_web_opfs_open: (ctx, pathPtr, resultPtr) => {
    var path = UTF8ToString(pathPtr);
    var finish = (handle, size, mode, message) => {
      growMemViews();
      HEAP32[resultPtr >> 2] = handle;
      HEAP32[(resultPtr >> 2) + 1] = handle >= 0 ? 0 : 1;
      HEAPF64[(resultPtr + 8) >> 3] = size;
      stringToUTF8(mode, resultPtr + 16, 16);
      stringToUTF8(message, resultPtr + 32, 256);
      _emscripten_proxy_finish(ctx);
    };
    PcsxOpfs.open(path).then(
      (opened) => finish(opened.handle, opened.size, opened.mode, ""),
      (error) => finish(-1, 0, "", (error && error.name ? error.name + ": " : "") + (error && error.message ? error.message : String(error))),
    );
  },

  // Reads up to length bytes at offset into dst; the count (short at end of file) or -1.
  pcsx2_web_opfs_read__deps: ['$PcsxOpfs'],
  pcsx2_web_opfs_read: (handle, dst, offset, length) => {
    var file = PcsxOpfs.files.get(handle);
    if (!file) return -1;
    if (!Number.isSafeInteger(offset) || offset < 0 || length < 0) return -1;
    if (length === 0) return 0;
    try {
      growMemViews();
      var target = HEAPU8.subarray(dst, dst + length);
      var count = 0;
      while (count < length) {
        var got = file.access.read(target.subarray(count, length), { at: offset + count });
        if (!got) break;
        count += got;
      }
      return count;
    } catch (error) {
      err("OPFS read of " + file.path + " at " + offset + " failed: " + error);
      return -1;
    }
  },

  pcsx2_web_opfs_close__deps: ['$PcsxOpfs'],
  pcsx2_web_opfs_close: (handle) => {
    var file = PcsxOpfs.files.get(handle);
    if (!file) return;
    PcsxOpfs.files.delete(handle);
    try {
      file.access.close();
    } finally {
      file.release();
    }
  },
});
