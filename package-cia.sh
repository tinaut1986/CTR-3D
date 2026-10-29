#!/usr/bin/env bash
# Build an installable CIA (FBI) from the 3DS build tree.
# Usage: ./package-cia.sh [build-dir]   (default: build-3ds)
# Needs makerom + bannertool: via MAKEROM / BANNERTOOL, on PATH, in $DEVKITPRO/tools/bin,
# in ./tools/bin, or in the sibling zelda-tmc-3ds checkout (platform/3ds/tools/bin).
# Normally run by ./build3ds.sh (which builds the .3dsx too).
# APP_SYSTEM_MODE_EXT=124MB|178MB picks the New 3DS app memory mode (default 178MB).
# APP_SYSTEM_MODE=64MB|72MB|80MB|96MB is the legacy (Old 3DS/2DS) mode (default 64MB).
# O3DS_SIM=1 builds the Old-3DS SIMULATION CIA (ctr3d-o3dssim.cia): its own title ID
#   (0x5A2C4, installs next to the normal one and shares sdmc:/3ds/ctr_native/),
#   SystemModeExt Legacy + SystemMode ${O3DS_MEM:-80MB}, CpuSpeed 268MHz, no L2 cache.
#   On a New 3DS the game sees a legacy-mode region and runs as an Old 3DS would
#   (clock-up off, no core 2/3 workers); see docs/3DS_PORT.md. Not for release.
set -euo pipefail
cd "$(dirname "$(readlink -f "$0")")"

BUILD="${1:-build-3ds}"
DEVKITPRO="${DEVKITPRO:-/opt/devkitpro}"
ELF="${BUILD}/ctr_native.elf"
SMDH="${BUILD}/ctr_native.smdh"
OUT="${BUILD}/cia"
CIA_NAME="ctr3d"
if [[ "${O3DS_SIM:-0}" == "1" ]]; then
    APP_SYSTEM_MODE_EXT="Legacy"
    APP_SYSTEM_MODE="${O3DS_MEM:-80MB}"
    APP_CPU_SPEED="268MHz"
    APP_L2_CACHE="false"
    APP_UNIQUE_ID="0x5A2C4"
    APP_PRODUCT_CODE="CTR-P-CT3S"
    APP_TITLE="CTR 3D O3DS SIM"
    CIA_NAME="ctr3d-o3dssim"
fi
APP_SYSTEM_MODE_EXT="${APP_SYSTEM_MODE_EXT:-178MB}"
APP_SYSTEM_MODE="${APP_SYSTEM_MODE:-64MB}"
APP_CPU_SPEED="${APP_CPU_SPEED:-804MHz}"
APP_L2_CACHE="${APP_L2_CACHE:-true}"
APP_UNIQUE_ID="${APP_UNIQUE_ID:-0x5A2C3}"
APP_PRODUCT_CODE="${APP_PRODUCT_CODE:-CTR-P-CT3D}"
APP_TITLE="${APP_TITLE:-CTR 3D}"

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

"${MAKEROM}" -f cia -o "${OUT}/${CIA_NAME}.cia" -DAPP_ENCRYPTED=false -DAPP_SYSTEM_MODE_EXT="${APP_SYSTEM_MODE_EXT}" \
    -DAPP_SYSTEM_MODE="${APP_SYSTEM_MODE}" -DAPP_CPU_SPEED="${APP_CPU_SPEED}" -DAPP_L2_CACHE="${APP_L2_CACHE}" \
    -DAPP_UNIQUE_ID="${APP_UNIQUE_ID}" -DAPP_PRODUCT_CODE="${APP_PRODUCT_CODE}" -DAPP_TITLE="${APP_TITLE}" \
    -elf "${ELF}" -rsf platform/n3ds/cia/ctr3d.rsf -exefslogo -target t \
    -icon "${SMDH}" -banner "${OUT}/banner.bnr" -major 1 -minor 0 -micro 0
echo "Built ${OUT}/${CIA_NAME}.cia (SystemModeExt ${APP_SYSTEM_MODE_EXT}, SystemMode ${APP_SYSTEM_MODE}, ${APP_CPU_SPEED}, L2 ${APP_L2_CACHE}, UniqueId ${APP_UNIQUE_ID})"
