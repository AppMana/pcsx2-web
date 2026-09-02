#!/usr/bin/env bash
# Rebuilds the homebrew PS2 test fixtures under web/tests/fixtures with the
# pinned ps2dev toolchain image and checks that every output is a valid EE ELF.
# The built .elf files are committed next to their sources so test lanes never
# need the toolchain; run this script only when a fixture source changes.
#
# Usage: web/scripts/build-fixtures.sh [fixture ...]
set -euo pipefail

# ps2dev/ps2dev:latest as pulled on 2026-09-01. The digest is what makes the
# build reproducible; the tag is recorded only for reference.
IMAGE_TAG="ps2dev/ps2dev:latest"
IMAGE_DIGEST="sha256:4b0ef59ef3b2f127fb87ff7e2f2178113d2d35d4964ec8f261c6f1b9c7593861"
IMAGE="ps2dev/ps2dev@${IMAGE_DIGEST}"
# The ps2dev image carries the toolchain but no make, so the fixtures build in
# a derived image that only adds Alpine's make on top of the pinned digest.
LOCAL_IMAGE="pcsx2-web-fixtures:${IMAGE_DIGEST#sha256:}"

WEB_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
FIXTURE_DIR="${WEB_DIR}/tests/fixtures"
ALL_FIXTURES=(hello_tty gs_sprite gs_blend vu1_cube)

if [ "$#" -gt 0 ]; then
	FIXTURES=("$@")
else
	FIXTURES=("${ALL_FIXTURES[@]}")
fi

ensure_image() {
	if docker image inspect "${LOCAL_IMAGE}" >/dev/null 2>&1; then
		return
	fi
	docker build -t "${LOCAL_IMAGE}" - <<DOCKERFILE
FROM ${IMAGE}
RUN apk add --no-cache make
DOCKERFILE
}

run_in_image() {
	local workdir="$1"
	shift
	docker run --rm \
		--user "$(id -u):$(id -g)" \
		-e HOME=/tmp \
		-v "${FIXTURE_DIR}:/src" \
		-w "/src/${workdir}" \
		"${LOCAL_IMAGE}" "$@"
}

echo "toolchain image: ${IMAGE_TAG} (${IMAGE})"
ensure_image

for name in "${FIXTURES[@]}"; do
	if [ ! -f "${FIXTURE_DIR}/${name}/Makefile" ]; then
		echo "no such fixture: ${name}" >&2
		exit 1
	fi
	echo "== building ${name}"
	run_in_image "${name}" make clean all
	echo "== verifying ${name}/${name}.elf"
	run_in_image "${name}" sh -c "
		mips64r5900el-ps2-elf-readelf -h ${name}.elf | grep -E 'Class|Data|Type|Machine|Entry point'
		mips64r5900el-ps2-elf-readelf -h ${name}.elf | grep -q 'Class: *ELF32'
		mips64r5900el-ps2-elf-readelf -h ${name}.elf | grep -q \"little endian\"
		mips64r5900el-ps2-elf-readelf -h ${name}.elf | grep -q 'Machine: *MIPS R3000'
		mips64r5900el-ps2-elf-readelf -h ${name}.elf | grep -q 'Type: *EXEC'
		mips64r5900el-ps2-elf-readelf -l ${name}.elf | grep -q 'LOAD'
	"
	# Common objects are rebuilt by the next fixture; remove them so a stale
	# object from a different include path can never be linked in.
	rm -f "${FIXTURE_DIR}"/common/*.o
done

echo "== checksums"
(cd "${FIXTURE_DIR}" && for name in "${FIXTURES[@]}"; do sha256sum "${name}/${name}.elf"; done)
