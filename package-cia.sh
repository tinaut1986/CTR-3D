#!/usr/bin/env bash
# Build an installable CIA (FBI) from the 3DS build tree.
# Usage: ./package-cia.sh [build-dir]   (default: build-3ds)
# Needs makerom + bannertool: via MAKEROM / BANNERTOOL, on PATH, in $DEVKITPRO/tools/bin,
# in ./tools/bin, or in the sibling zelda-tmc-3ds checkout (platform/3ds/tools/bin).
# Normally run by ./build3ds.sh (which builds the .3dsx too).
# APP_SYSTEM_MODE_EXT=124MB|178MB picks the New 3DS app memory mode (default 178MB).
set -euo pipefail
cd "$(dirname "$(readlink -f "$0")")"

BUILD="${1:-build-3ds}"
DEVKITPRO="${DEVKITPRO:-/opt/devkitpro}"
ELF="${BUILD}/ctr_native.elf"
SMDH="${BUILD}/ctr_native.smdh"
OUT="${BUILD}/cia"
export APP_SYSTEM_MODE_EXT="${APP_SYSTEM_MODE_EXT:-178MB}"

find_tool() {
    local name="$1" var="${2:-}"
    if [[ -n "${var}" && -x "${var}" ]]; then echo "${var}"; return; fi
    command -v "${name}" 2>/dev/null && return
    local dir
    for dir in "${DEVKITPRO}/tools/bin" "tools/bin" "../zelda-tmc-3ds/platform/3ds/tools/bin"; do
        if [[ -x "${dir}/${name}" ]]; then echo "${dir}/${name}"; return; fi
    done
}
MAKEROM="$(find_tool makerom "${MAKEROM:-}")"
BANNERTOOL="$(find_tool bannertool "${BANNERTOOL:-}")"
[[ -n "${MAKEROM}" && -n "${BANNERTOOL}" ]] || { echo "ERROR: makerom/bannertool not found (set MAKEROM / BANNERTOOL)." >&2; exit 1; }
[[ -f "${ELF}" && -f "${SMDH}" ]] || { echo "ERROR: ${ELF} or ${SMDH} missing; build for 3DS first (see README.md)." >&2; exit 1; }

mkdir -p "${OUT}"
# Banner: use BANNER_PNG / BANNER_WAV if given, else a plain placeholder.
BANNER_PNG="${BANNER_PNG:-${OUT}/banner.png}"
BANNER_WAV="${BANNER_WAV:-${OUT}/silence.wav}"
if [[ ! -f "${BANNER_PNG}" || ! -f "${BANNER_WAV}" ]]; then
python3 - "${BANNER_PNG}" "${BANNER_WAV}" <<'PY'
import sys, zlib, struct, wave
png, wav = sys.argv[1:3]
w, h = 256, 128
raw = b''.join(b'\x00' + bytes([20, 60, 140]) * w for _ in range(h))
def ch(t, d):
    c = struct.pack('>I', len(d)) + t + d
    return c + struct.pack('>I', zlib.crc32(t + d) & 0xffffffff)
open(png, 'wb').write(b'\x89PNG\r\n\x1a\n' + ch(b'IHDR', struct.pack('>IIBBBBB', w, h, 8, 2, 0, 0, 0)) + ch(b'IDAT', zlib.compress(raw)) + ch(b'IEND', b''))
f = wave.open(wav, 'wb'); f.setnchannels(2); f.setsampwidth(2); f.setframerate(32728); f.writeframes(b'\0' * 4 * 32728); f.close()
PY
fi
"${BANNERTOOL}" makebanner -i "${BANNER_PNG}" -a "${BANNER_WAV}" -o "${OUT}/banner.bnr"

"${MAKEROM}" -f cia -o "${OUT}/ctr3d.cia" -DAPP_ENCRYPTED=false -DAPP_SYSTEM_MODE_EXT="${APP_SYSTEM_MODE_EXT}" \
    -elf "${ELF}" -rsf platform/n3ds/cia/ctr3d.rsf -exefslogo -target t \
    -icon "${SMDH}" -banner "${OUT}/banner.bnr" -major 1 -minor 0 -micro 0
echo "Built ${OUT}/ctr3d.cia (SystemModeExt ${APP_SYSTEM_MODE_EXT})"
