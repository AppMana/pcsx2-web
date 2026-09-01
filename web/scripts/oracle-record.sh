#!/usr/bin/env bash
# Records a native oracle trace for one fixture with pcsx2-tracerunner and
# checks that it is deterministic by running it twice and comparing the
# per-frame CPU hashes, the TTY output and (when enabled) the audio hashes.
# The first run's trace directory is kept as the recorded oracle.
#
# Usage: web/scripts/oracle-record.sh [options] <game.iso|program.elf> [-- tracerunner args...]
#   -o <dir>     trace output directory (default: web/tests/oracle/<image title>)
#   -b <dir>     BIOS directory (default: $PCSX2_BIOS_DIR)
#   -n <frames>  number of frames to run (default: 300)
#   -r <name>    tracerunner binary (default: $PCSX2_TRACERUNNER or build-native/bin/pcsx2-tracerunner)
set -euo pipefail

REPO_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
OUT_DIR=""
BIOS_DIR="${PCSX2_BIOS_DIR:-}"
FRAMES=300
RUNNER="${PCSX2_TRACERUNNER:-${REPO_DIR}/build-native/bin/pcsx2-tracerunner}"

while getopts "o:b:n:r:" opt; do
	case "${opt}" in
		o) OUT_DIR="${OPTARG}" ;;
		b) BIOS_DIR="${OPTARG}" ;;
		n) FRAMES="${OPTARG}" ;;
		r) RUNNER="${OPTARG}" ;;
		*) exit 2 ;;
	esac
done
shift $((OPTIND - 1))

if [ "$#" -lt 1 ]; then
	echo "usage: $0 [-o dir] [-b bios] [-n frames] [-r runner] <image> [-- tracerunner args...]" >&2
	exit 2
fi

IMAGE="$1"
shift
if [ "${1:-}" = "--" ]; then
	shift
fi
EXTRA_ARGS=("$@")

if [ ! -x "${RUNNER}" ]; then
	echo "tracerunner not found at ${RUNNER}" >&2
	exit 1
fi
if [ -z "${BIOS_DIR}" ]; then
	echo "no BIOS directory: pass -b or set PCSX2_BIOS_DIR" >&2
	exit 1
fi
if [ -z "${OUT_DIR}" ]; then
	TITLE="$(basename "${IMAGE}")"
	TITLE="${TITLE%.*}"
	OUT_DIR="${REPO_DIR}/web/tests/oracle/${TITLE}"
fi

RUN2_DIR="${OUT_DIR}.determinism"
rm -rf "${OUT_DIR}" "${RUN2_DIR}"

record() {
	"${RUNNER}" -bios "${BIOS_DIR}" -surfaceless -frames "${FRAMES}" -trace "$1" "${EXTRA_ARGS[@]}" -- "${IMAGE}"
}

record "${OUT_DIR}"
record "${RUN2_DIR}"

status=0
for name in cpu.jsonl tty.txt audio.jsonl; do
	if [ ! -e "${OUT_DIR}/${name}" ] && [ ! -e "${RUN2_DIR}/${name}" ]; then
		continue
	fi
	if cmp -s "${OUT_DIR}/${name}" "${RUN2_DIR}/${name}"; then
		echo "${name}: identical"
	else
		echo "${name}: DIFFERS"
		diff "${OUT_DIR}/${name}" "${RUN2_DIR}/${name}" | head -n 20
		status=1
	fi
done

rm -rf "${RUN2_DIR}"
exit "${status}"
