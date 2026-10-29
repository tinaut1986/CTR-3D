# Project notes for Claude

CTR 3D: a Nintendo 3DS port of **Crash Team Racing**, built on CTR Native (the PC
port over the CTR-ModSDK decompilation). The 3DS layer is C on devkitARM, libctru
and citro3d (`/opt/devkitpro`). This is the owner's repository
(`tinaut1986/CTR-3D`); `upstream` is `zando95/CTR-3D`. The owner ships other 3DS
ports with the same release scheme (`../zelda-tmc-3ds`, `../mzm`, `../sm-3ds`,
`../legend-of-doom-3ds`, `../RSDKv5-Decompilation`): this file and the release and
updater code are copied from `../zelda-tmc-3ds`. When in doubt, do what it does.

## Language

- Chat with the owner in **Spanish (castellano)**.
- Code, comments, identifiers, commit messages and docs: **English**.

## Git and GitHub

- **Every push, PR, issue and release goes to `tinaut1986/CTR-3D`** (`origin`).
  Never to `zando95/CTR-3D` (`upstream`, its push URL is `no_push`).
- Never push, tag, or send anything to a console unless the owner asks.

### Branches

- `main` = stable. Only receives `--no-ff` merges of a finished release branch.
- `release/vX.Y.Z` = the current release line (beta); work accumulates here, and
  being on it means "not stable yet".
- Topic branches (`feat/...`, `fix/...`, `perf/...`, `chore/...`, `docs/...`) are
  cut from the active release branch and merged back into it with `--no-ff`.
  **One topic per branch, always cut from the release branch, never from another
  topic branch.** The flow is topic -> `release/vX.Y.Z` -> `main`.
- Never commit directly on `main` or `release/*`. Commit only when the owner asks.

### Versions

`vX.Y.Z`, started at `v0.1.0`. Minor = milestone, patch = fix round. Ask before a
minor bump; never infer one.

- The version is derived from git by `platform/n3ds/tools/version.sh`: an exact
  `vX.Y.Z` tag gives `X.Y.Z`; anything on (or cut from) `release/vX.Y.Z` gives
  `X.Y.Z-dev.<commits since main>[.<commits since the release branch>]+<hash>`.
  Nothing to bump by hand. CMake runs it at configure time and writes
  `build-*/generated/ctr_version.h` (`CTR_PORT_VERSION`, `CTR_PORT_VERSION_LABEL`,
  `CTR_PORT_IS_BETA`); the label also reaches the `[BUILD]` log line, the SMDH
  description, the bottom screen and `package-cia.sh` (`-major/-minor/-micro`).
- **Only three-component tags are ours.** The older tag `beta-1.0` (and upstream's
  tags) are in this repository too; the version script, the release workflow
  (`v[0-9]+.[0-9]+.[0-9]+`) and `git describe --match` all ignore them. Never create
  a two-component tag or reuse `beta-1.0`.
- Title ID `0004000005A2C300` (`platform/n3ds/cia/ctr3d.rsf`), data folder
  `sdmc:/3ds/ctr_native/`.

### Releases (installer on GitHub)

`.github/workflows/build-release.yml` builds the CIA on any `vX.Y.Z` tag push (and
on manual dispatch) and publishes a GitHub Release with it and a QR code. No 3DSX:
from the Homebrew Launcher the game does not get the New 3DS' full memory;
`build3ds.sh` still makes one locally. As in ../zelda-tmc-3ds, betas carry the debug
tools (`./build3ds.sh`, `CTR_INTERNAL` on: Debug tab, full FTP server) and stable
releases are the production configuration (`./build3ds.sh --release`,
`CTR_INTERNAL=OFF`; FTP only with `dev-ftp.txt`, install-only). The channel depends
on whether `main` can reach the tagged commit:

| Built from | Channel |
|---|---|
| Tag on an unmerged `release/*` branch | `Beta` (pre-release) |
| Tag on `main`, or on a commit merged into `main` | `Release` |
| Manual dispatch on any branch | `Beta` |

`.github/workflows/build-3ds.yml` only compiles (debug and production configs, and
packages both CIAs as workflow artifacts) for pull requests and pushes to `main`
and `release/**`; it publishes nothing.

The CIA holds the code only. The game's data is not redistributable: it stays on
the player's SD card (`sdmc:/3ds/ctr_native/`), and the workflows fail if anything
under `assets/` or a ROM/disc image/save/build output is tracked by git.

**First, for either path:** write `docs/release-notes/<tag>.md` on a topic branch, show
it to the owner and merge it into the release branch **before** the tag exists. Do it
unprompted whenever a tag is about to be made.

**Beta**: tag the release branch, push branch then tag.

```sh
git tag -a v0.1.0 -m "v0.1.0"
git push origin release/v0.1.0
git push origin v0.1.0
```

**Stable**: merge into `main`, tag the merge, push **main before the tag** (the tag
build checks reachability from `origin/main`).

```sh
git checkout main
git merge --no-ff release/v0.1.0
git tag -a v0.1.0 -m "v0.1.0"
git push origin main
git push origin v0.1.0
```

A tag already shipped as a beta is promoted by re-running the workflow from the
Actions tab **on the tag itself** (not on `main`). That is only valid if **nothing was
committed after the beta's tag**; otherwise tag the next version.

### Release notes (shown in the updater)

One file per tag, `docs/release-notes/vX.Y.Z.md` (rules in its README): 3 to 6 short
lines, each starting with `- `, plain text for the player, in English. The CI copies
it into the release body between `<!-- ctr3d-notes -->` markers; the console's updater
shows that block (`Update_FormatNotes` in `platform/n3ds/update/update_manifest.c`;
the marker must stay identical in both places), or the commit list when the block is
missing.

A **stable** release lists everything since the previous *stable* tag, betas included;
a **beta** lists only what is new since the previous tag. The console's updater reads
the release list: with + BETAS it offers the newest of betas and stable releases,
with STABLE the newest stable one, and its notes join those of every release of the
channel newer than the installed build (newest first). A stable player never sees
beta pages, which is why a stable's notes must cover its betas.

### The channel is baked into the binary

The workflow passes `CTR_CHANNEL=beta|release` (and `CTR_VERSION=X.Y.Z`) to
`./build3ds.sh --release`. The generated `ctr_version.h` then carries
`CTR_PORT_IS_BETA` and `CTR_PORT_VERSION_LABEL` ("X.Y.Z BETA"); `CTR_PORT_VERSION`
stays the plain number (user-agent, file names, comparisons). The updater orders one
number's builds `X.Y.Z-dev.N` < `X.Y.Z BETA` < `X.Y.Z`: a dev build is offered the
beta of its number (it predates the tag, the branch being renamed after it), an
installed beta the stable release, and a beta is never offered its own pre-release
page. The UPDATER tab shows the release found with BETA when it is one, and the
installed build's label.

### After tagging

After **every** tag, beta or stable, rename the release branch to the next patch at
once (before more work or any build) and delete the old remote branch; otherwise dev
builds call themselves `X.Y.Z-dev.N` and the console offers the already published
`X.Y.Z` as an update. Do the steps one at a time and check with `git ls-remote`.

```sh
git branch -m release/v0.1.0 release/v0.1.1
git branch --unset-upstream
git push -u origin release/v0.1.1
git push origin --delete release/v0.1.0
```

## Game data and legal

- Never commit, package or upload game data (disc images, extracted assets, saves),
  or any build that contains it. `.gitignore` covers `assets/`, `memcards/`, `debug/`.
- Licenses and notices: `LICENSE`, `THIRD_PARTY_NOTICES.md` (the updater links
  curl, mbedTLS and jansson and embeds the Mozilla CA bundle; see
  `platform/n3ds/update/deps/`).

## Build

Toolchain in `/opt/devkitpro` (devkitARM, libctru, citro3d).

```sh
export DEVKITPRO=/opt/devkitpro DEVKITARM=/opt/devkitpro/devkitARM
./build3ds.sh                    # debug config (CTR_INTERNAL on) -> build-3ds/, 3DSX + CIA
./build3ds.sh --release          # production config (CTR_INTERNAL off) -> build-3ds-release/
./build3ds.sh --install 192.168.1.50   # build, send the CIA to the running game, install, relaunch
```

Both configs always contain the updater. `makerom` and `bannertool` are committed
in `platform/n3ds/tools/bin/` (from `../zelda-tmc-3ds`), which is also how CI gets
them. The updater's libcurl/mbedTLS/jansson come from the devkitPro portlibs when
those include jansson, else the pinned sources are built once into
`build-update-deps/` by `platform/n3ds/tools/build_update_deps.py` (needs network).

**Enum ABI:** the game TU is built with `-fno-short-enums` (its struct layouts assume
4-byte enums); every libctru-facing TU, including the updater, the CIA installer and
the manifest parser (jansson embeds an enum in `json_t`), is built with
`-fshort-enums`, listed in `CMakeLists.txt`. A new TU that includes libctru, curl or
jansson headers goes in that list, and nothing enum-typed crosses into the game TU.

## Updater and install paths

- `platform/n3ds/update/updater.c` (network thread at priority 0x3F on a spare core,
  libcurl + mbedTLS, SHA-256 and title-ID checks), `update_manifest.c` (GitHub
  release list, version order, notes) and the UPDATER tab in `n3ds_bottomui.c`.
  Checking and downloading happen only when the player asks (or AUTO CHECK at
  boot, off by default); the channel (`betas`) and AUTO CHECK (`autoupdate`) are in
  `ctr3d.cfg`. Downloads stream to `sdmc:/3ds/ctr_native/update/`.
- Installing is `N3DS_CiaInstall` (`n3ds_ciainstall.c`), the same AM path as the FTP
  server's `SITE INSTALL` (`n3ds_ftp.c`); the restart is the FTP relaunch
  (`N3DS_FtpScheduleRelaunch` then `N3DS_FtpRelaunchDue` in the main loop). Do not
  add a second install or relaunch path.
- Host test: `platform/n3ds/tests/run-update-tests.py --jansson-prefix DIR` (see its
  header for building a host Jansson).
- Untested on hardware until the owner runs it on a console; Azahar can check the
  logic but says nothing about timings.
