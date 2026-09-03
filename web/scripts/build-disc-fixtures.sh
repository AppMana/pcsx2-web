#!/usr/bin/env bash
# Builds the disc image fixtures under web/tests/fixtures from the committed
# homebrew ELFs: an ISO 9660 image whose SYSTEM.CNF boots the ELF through the
# BIOS (BOOT2 = cdrom0:\NAME.ELF;1) and a CHD of that ISO. The images are
# committed next to their test.toml so test lanes never need these tools; run
# this script only when a source ELF changes.
#
# Requires xorriso (ISO) and chdman from mame-tools (CHD). Two consecutive
# runs produce byte identical images: the ISO timestamps come from
# SOURCE_DATE_EPOCH and chdman writes no timestamps.
#
# Usage: web/scripts/build-disc-fixtures.sh
set -euo pipefail

WEB_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
FIXTURE_DIR="${WEB_DIR}/tests/fixtures"
export SOURCE_DATE_EPOCH="${SOURCE_DATE_EPOCH:-1756684800}"

# build_disc <fixture dir> <output name> <source elf> <name on disc> <volume id>
build_disc() {
	local out_dir="$1" name="$2" elf="$3" disc_name="$4" volume="$5"
	local stage
	stage="$(mktemp -d)"
	trap 'rm -rf "${stage}"' RETURN
	printf 'BOOT2 = cdrom0:\\%s;1\r\nVER = 1.00\r\nVMODE = NTSC\r\n' "${disc_name}" > "${stage}/SYSTEM.CNF"
	cp "${elf}" "${stage}/${disc_name}"
	touch -d "@${SOURCE_DATE_EPOCH}" "${stage}/SYSTEM.CNF" "${stage}/${disc_name}"
	mkdir -p "${out_dir}"
	xorriso -as mkisofs -quiet -iso-level 1 -V "${volume}" -sysid PLAYSTATION \
		-o "${out_dir}/${name}.iso" "${stage}"
	rm -f "${out_dir}/${name}.chd"
	chdman createdvd -i "${out_dir}/${name}.iso" -o "${out_dir}/${name}.chd" >/dev/null
	chdman verify -i "${out_dir}/${name}.chd" >/dev/null
}

build_disc "${FIXTURE_DIR}/hello_tty_iso" hello_tty "${FIXTURE_DIR}/hello_tty/hello_tty.elf" HELLO.ELF HELLO_TTY

echo "== checksums"
(cd "${FIXTURE_DIR}" && sha256sum hello_tty_iso/hello_tty.iso hello_tty_iso/hello_tty.chd)
