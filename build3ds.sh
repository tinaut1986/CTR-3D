#!/usr/bin/env bash
# Builds the 3DS target: the .3dsx AND the installable .cia (the CIA is what the
# author runs: a .3dsx under the Homebrew Launcher only gets the host title's
# memory, the CIA's SystemModeExt 124MB gets ~105 MB of the New 3DS app region).
#
#   ./build3ds.sh                       build/refresh both
#   ./build3ds.sh --no-cia              .3dsx only
#   ./build3ds.sh --release             the production config instead of the debug one:
#                                       -DCTR_INTERNAL=OFF in build-3ds-release/ (what the
#                                       release workflow builds)
#   ./build3ds.sh --install [IP[:PORT]] build, then send the CIA to the running game's
#                                       FTP server and have it install + relaunch
#                                       (IP is remembered in .3ds_ftp_ip; port default 5000).
#                                       The game must be running as an installed CIA with
#                                       FTP on: debug build = on by default, production
#                                       build = an empty dev-ftp.txt in /3ds/ctr_native/.
#
# Environment (set by .github/workflows/build-release.yml; empty for local builds):
#   CTR_CHANNEL=beta|release   baked into the binary ("0.1.0 BETA" label, updater channel)
#   CTR_VERSION=X.Y.Z          override the version derived from git (platform/n3ds/tools/version.sh)
#   UPDATE_DEPS_ROOT=DIR       prefix with curl, mbedtls and jansson for the updater (default: the
#                              devkitPro portlibs, else a pinned build into build-update-deps/)
#
# Outputs: build-3ds/ctr_native.3dsx and build-3ds/cia/ctr3d.cia (+ ctr3d-vX.Y.Z.cia).
# Run via an MSYS2 login shell on Windows (`bash.exe -l build3ds.sh`). The cd comes
# first because a login shell starts in $HOME, so a bare `cmake --build` would run
# against the wrong directory.
cd "$(dirname "$(readlink -f "$0")")" || exit 2

BUILD=build-3ds
INTERNAL=ON
DEVKITPRO="${DEVKITPRO:-/opt/devkitpro}"
CIA_PATH="/3ds/ctr_native/update/dev.cia"
DO_CIA=1
INSTALL=0
HOST=""
while [[ $# -gt 0 ]]; do
    case "$1" in
        --no-cia) DO_CIA=0 ;;
        --release) BUILD=build-3ds-release; INTERNAL=OFF ;;
        --install) INSTALL=1; DO_CIA=1
            if [[ $# -gt 1 && "$2" != --* ]]; then HOST="$2"; shift; fi ;;
        -h|--help) sed -n '2,28p' "$0"; exit 0 ;;
        *) echo "unknown option: $1 (see --help)" >&2; exit 2 ;;
    esac
    shift
done

# The updater's libraries: curl + mbedTLS + jansson. The portlibs have them on some installs;
# otherwise the pinned sources are built once into build-update-deps/ (needs network).
if [[ -z "${UPDATE_DEPS_ROOT:-}" ]]; then
    UPDATE_DEPS_ROOT="${DEVKITPRO}/portlibs/3ds"
    if [[ ! -f "${UPDATE_DEPS_ROOT}/lib/libcurl.a" || ! -f "${UPDATE_DEPS_ROOT}/lib/libjansson.a" ]]; then
        UPDATE_DEPS_ROOT="$PWD/build-update-deps/prefix"
        if [[ ! -f "${UPDATE_DEPS_ROOT}/lib/libcurl.a" || ! -f "${UPDATE_DEPS_ROOT}/lib/libjansson.a" ]]; then
            python3 platform/n3ds/tools/build_update_deps.py "$PWD/build-update-deps" || exit $?
        fi
    fi
fi

if [[ ! -f "${BUILD}/CMakeCache.txt" ]]; then
    cmake -S . -B "${BUILD}" -DCMAKE_TOOLCHAIN_FILE="${DEVKITPRO}/cmake/3DS.cmake" -DCMAKE_BUILD_TYPE=Release \
        -DCTR_INTERNAL=${INTERNAL} -DUPDATE_DEPS_ROOT="${UPDATE_DEPS_ROOT}" \
        -DCTR_CHANNEL="${CTR_CHANNEL:-}" -DCTR_VERSION="${CTR_VERSION:-}" 2>&1 || exit $?
else
    # Re-evaluate the version (a new commit or tag) and the channel on every run.
    cmake -S . -B "${BUILD}" -DUPDATE_DEPS_ROOT="${UPDATE_DEPS_ROOT}" -DCTR_CHANNEL="${CTR_CHANNEL:-}" -DCTR_VERSION="${CTR_VERSION:-}" > /dev/null 2>&1 || exit $?
fi
cmake --build "${BUILD}" 2>&1
rc=${PIPESTATUS[0]}
[[ $rc -eq 0 ]] || exit $rc
echo "Built ${BUILD}/ctr_native.3dsx"

if [[ $DO_CIA -eq 1 ]]; then
    ./package-cia.sh "${BUILD}" || exit $?
fi

if [[ $INSTALL -eq 1 ]]; then
    [[ -z "$HOST" && -f .3ds_ftp_ip ]] && HOST="$(cat .3ds_ftp_ip)"
    [[ -n "$HOST" ]] || { echo "ERROR: --install needs the 3DS address (shown on the Updater tab): ./build3ds.sh --install 192.168.1.50" >&2; exit 2; }
    [[ "$HOST" == *:* ]] || HOST="${HOST}:5000"
    echo "$HOST" > .3ds_ftp_ip
    CIA="${BUILD}/cia/ctr3d.cia"
    SIZE=$(stat -c %s "$CIA" 2>/dev/null || stat -f %z "$CIA")
    echo "Sending ${CIA} (${SIZE} bytes) to ftp://${HOST} and installing..."
    # "+ALLO" (before the transfer) gives the game the total for its progress bar;
    # "-SITE INSTALL" (after it) installs the CIA over the running title, which then
    # restarts itself after a 10 s countdown shown on the bottom screen.
    curl --max-time 300 --ftp-create-dirs -T "$CIA" "ftp://${HOST}${CIA_PATH}" \
         -Q "+ALLO ${SIZE}" -Q "-SITE INSTALL ${CIA_PATH}" || exit $?
    echo "Installed; the game relaunches in about 10 s."
fi
