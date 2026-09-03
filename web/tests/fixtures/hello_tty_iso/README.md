# hello_tty_iso

The `hello_tty` program on a disc: an ISO 9660 image whose `SYSTEM.CNF` boots
`hello_tty.elf` through the BIOS, and a CHD of that image. Both are built by
`web/scripts/build-disc-fixtures.sh` from the committed `../hello_tty/hello_tty.elf`
and are committed here so test lanes never need xorriso or chdman.

## Layout

    SYSTEM.CNF   BOOT2 = cdrom0:\HELLO.ELF;1  VER = 1.00  VMODE = NTSC  (CRLF)
    HELLO.ELF    ../hello_tty/hello_tty.elf

`xorriso -as mkisofs -iso-level 1 -V HELLO_TTY -sysid PLAYSTATION` with
`SOURCE_DATE_EPOCH=1756684800`, so the 552,960 byte image (270 sectors, 150 of
them mkisofs padding) is byte identical between runs. PCSX2 detects it as a
2048 byte per sector CD (`InputIsoFile::Detect`), reads `SYSTEM.CNF` through
`IsoReader`, and with fast boot the BIOS loader fetches `cdrom0:\HELLO.ELF;1`
through the emulated CDVD, so every sector goes through the CDVD file reader.

`hello_tty.chd` is `chdman createdvd -i hello_tty.iso` (MAME 0.264, 2048 byte
units, 4096 byte hunks, lzma/zlib/huffman/flac); chdman writes no timestamps,
so it is reproducible as well.

## Oracle

`expected/` is recorded from the ISO with `web/scripts/oracle-record.sh -n 600
... -- -renderer sw -trace-ram-every 100 -png-every 100 -audio-hash`. The
CHD, recorded the same way, gives byte identical `cpu.jsonl`, `tty.txt` and
`audio.jsonl`, so both images are compared against this one directory. Against
`../hello_tty/expected/tty.txt` only the three IOP loader lines differ
(`host:hello_tty.elf` becomes `cdrom0:HELLO.ELF;1`); every guest `KEY=VALUE`
line and every frame PNG is identical. `cpu.jsonl` differs from the ELF
fixture's because the boot path differs.

## In the browser

`disc-boot.spec.ts` imports both images from the fixture library into
origin-private storage on `storage.html`, then runs `/opfs/games/<image>` on
`runtime.html`. The core opens the file in place through
`pcsx2/CDVD/OpfsFileReader.cpp` (a sync access handle on the OPFS thread; CHD
through libchdr's `core_file` callbacks over the same handle) and the TTY and
CPU trace are compared with `expected/` exactly like the ELF fixtures.
