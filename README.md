# CTR 3D

A Nintendo 3DS homebrew port of Crash Team Racing (PS1, 1999), for the New 3DS and, since v0.1.0, the Old 3DS and 2DS too.

This is a fork of [CTR Native](https://github.com/CTR-tools/ctr-native), the native PC port of the game built on the [CTR-ModSDK](https://github.com/CTR-tools/CTR-ModSDK) decompilation. The 3DS platform layer and the renderer for the 3DS GPU come from [zando95/CTR-3D](https://github.com/zando95/CTR-3D), which first brought the game to the New 3DS. This fork adds Old 3DS / 2DS support, races unlocked to 60 FPS, the race HUD on the bottom screen, reworked stereoscopic 3D and an in-game updater.

On a New 3DS the game runs with stereoscopic 3D on the slider, a genuinely widescreen top screen, and the HUD and map on the bottom screen. Races reach about 45-60 FPS there and about 25-30 FPS on an Old 3DS / 2DS (the PS1 original ran at 30). Not every track and mode has been played through yet; bug reports are welcome.

CTR was also Nintendo's internal codename for the 3DS.

The PC port is still here and still builds; see [PC builds](#pc-builds-windows-and-linux) at the bottom. Everything above that section is about the 3DS.

## Philosophy

The goal is a game that could pass for a retail 3DS port: faithful to the original where it counts, and willing to leave PlayStation hardware quirks behind where the 3DS can do better.

- The track and the karts are transformed and textured by the 3DS GPU, so the PS1's vertex wobble and texture warping are gone.
- Performance wins over pixel-for-pixel parity. The close-up "mosaic" texture tier is not rendered and the full-screen blur effect is gone, because a steady frame rate matters more.
- Clear bugs in the original get fixed. The mirrored tires in ice reflections sit where the modellers intended, not where a PS1 branch delay slot put them.
- Enhancements are options, not mandates. Texture filtering exists, and defaults off, because players preferred crisp textures to mud.
- Nothing of the game ships with this project. You bring your own disc image.

## Requirements

- Any Nintendo 3DS family console: New 3DS, New 3DS XL, New 2DS XL, or the original 3DS, 3DS XL and 2DS. The Old 3DS / 2DS are slower (see above); menus run near 30 FPS there and the track select preview video at about 12 FPS.
- Custom firmware with the Homebrew Launcher ([Luma3DS](https://github.com/LumaTeam/Luma3DS)). If your console is not modded yet, [3ds.hacks.guide](https://3ds.hacks.guide/) is the place to start.
- A DSP firmware dump from your console, or the game will be silent. You do not have to make it yourself: if the audio cannot start, the game offers to extract it from your own console. (By hand: Rosalina menu, default **L + Down + Select**, Miscellaneous options, Dump DSP firmware.)
- Your own NTSC-U (North American) retail CTR disc image, in the common raw PSX BIN layout: MODE2/2352 sectors, data track starting at byte 0. A cooked 2048-byte `.iso` drops the XA and STR sectors that music, voices and video need.

## Install

1. Download `ctr3d-vX.Y.Z.cia` from the Releases page (or scan the QR code in the release) and install it with FBI. It appears on the HOME menu as CTR 3D.
2. Copy your disc image to `/3ds/ctr_native/assets/ctr-u.bin`.
3. Start it from the HOME menu. Data always lives in `/3ds/ctr_native/`.

Releases ship the CIA only: a `.3dsx` launched from the Homebrew Launcher does not get the New 3DS's full app memory. `./build3ds.sh` still builds one locally (see below).

The bottom screen's **Updater** tab can check GitHub for newer releases (stable, or stable plus betas), show what changed and install them itself.

Saves are written to `/3ds/ctr_native/memcards/`, in the same files the PC port uses, so progress can be copied between console and PC.

The Sony logo screen holds for several seconds after launch while the game's audio is cached; a `Loading audio` line shows its progress. On a slow SD card it takes longer. It happens once per launch.

## Playing

Buttons follow their position on the PlayStation pad, not their letter:

| 3DS | PS1 | In a race | In menus |
|---|---|---|---|
| B | Cross | Accelerate | Confirm |
| A | Circle | Use weapon | Confirm |
| Y | Square | Brake, reverse | Back |
| X | Triangle | Switch the top screen between speedometer and map | Back |
| L, R | L1, R1 | Hop, power-slide | |
| ZL | L2 | Change camera view | |
| ZR | R2 | Hold to look behind | |
| Circle Pad, D-Pad | D-pad | Steer | Navigate |
| START | Start | Pause | |

The screens:

- The bottom screen has touch tabs: Status, Map, Options and Updater. In a race, the Map tab shows the enlarged map with the HUD around it: clock, lap, fruit and wumpa count, speedometer, and the ranking of all 8 drivers with your own row highlighted. The top screen starts in speedometer mode; X puts the map there too, as Triangle does on PS1. In Adventure mode the hub map moves to the bottom screen as well.
- The Options tab has AUTO-HIDE HUD (the top screen drops the HUD while the Map tab is up), 3D EFFECT, FRAME SKIP, SHOW FPS, a colour theme and, on a New 3DS, CPU 804 MHZ.
- The 3D slider works in real time. On an Old 3DS / 2DS, 3D EFFECT defaults to AUTO and turns itself off if the game runs slow.

The main menu has an **OPTIONS** entry with four toggles: PAL PENTA (Penta Penguin's PAL/Japanese stats), RESERVES METER (the boost-reserves bar), BOTTOM MAP, and TEXTURE FILTER. They return to their defaults on every launch. VS and Battle modes are not on the menu; one console has one controller.

## Reporting a bug

Open an issue on the repository's Issues page. The bug-report form asks for what it needs: console model, Luma3DS version, SD card make and size, the version string shown under the game's name in the Homebrew Launcher, and what you were doing.

To attach a log, create an empty file named `enable-log.txt` in `/3ds/ctr_native/`, launch the game, reproduce the problem, then attach `/ctr3ds.log` from the SD card root. Logging costs a small stutter every couple of seconds, so delete `enable-log.txt` afterwards.

## Changes from the original game

Everything here is on by default unless it says otherwise. The toggles are the OPTIONS menu rows.

- **Widescreen.** The top screen renders a wider field of view, not a stretched 4:3 image.
- **Stereoscopic 3D** on the slider. The kart sits at the screen surface and the track goes into the screen; text and menus come out in whole pixels.
- **Races run unlocked to 60 FPS** instead of the PS1's 30. FRAME SKIP holds the game's speed on slower consoles by skipping some drawn frames instead of slowing down.
- **Old 3DS / 2DS support**, with faster level loads (a few seconds).
- **A HOME Menu icon, banner and banner sound** of its own.
- **The race HUD and the map on the bottom screen**, with the top-screen slot switching between speedometer and map. Toggle.
- **Penta Penguin has his PAL and Japanese stats**, the maximum in every class, instead of the NTSC-U ones. Toggle.
- **A boost-reserves meter** next to the power-slide meter, following the community ReservesMeter mod's design. Toggle.
- **Texture filtering.** Toggle, off by default.
- **Ice reflections no longer merge each kart's mirrored wheel pairs onto its centreline.** That is a bug in the shipped PS1 game.
- **The close-up "mosaic" texture tier is not rendered.** Near surfaces use the standard texture.
- **The Spyro 2 demo cheat code does something else now**, since a native build cannot launch the demo off the disc. One new cheat code has been added.
- **VS and Battle modes** are not offered.

The PC build from this repository carries the changes that are not 3DS-specific: PAL Penta, the reserves meter, the ice-reflection fix and the cheat codes. On PC they are always on; only the 3DS build has the OPTIONS menu.

## Building from source

### Nintendo 3DS

Install [devkitPro](https://devkitpro.org/) with the `3ds-dev` package group, then:

```
cmake -S . -B build-3ds -DCMAKE_TOOLCHAIN_FILE=$DEVKITPRO/cmake/3DS.cmake -DCMAKE_BUILD_TYPE=Release
cmake --build build-3ds
```

Or just `./build3ds.sh`, which configures if needed and builds both outputs: `build-3ds/ctr_native.3dsx` and the installable `build-3ds/cia/ctr3d.cia` (needs `makerom` and `bannertool`; see `package-cia.sh` for where it looks). The CIA is the preferred way to run it: it gets the New 3DS's full app memory (SystemModeExt 124MB), a .3dsx only gets the launching title's. `./build3ds.sh --install <3DS IP>` sends the CIA to the running game over FTP and installs it. That build includes the internal debug surface (FTP on by default, Debug tab); add `-DCTR_INTERNAL=OFF` to the configure line to build what the release ships (FTP only when an empty `dev-ftp.txt` is in `/3ds/ctr_native/`).

`./build3ds.sh --release` builds the production configuration (`CTR_INTERNAL=OFF`, `build-3ds-release/`), which is what a GitHub release ships. Versions come from git (`platform/n3ds/tools/version.sh`, tags `vX.Y.Z`); the game checks GitHub for newer releases from the Updater tab on the bottom screen and installs them itself. See `CLAUDE.md` for the release procedure.

On Windows, run both commands from devkitPro's MSYS2 shell using the `msys` `cmake` and `make` packages, not the mingw ones. From plain Git Bash the cross-compiler fails with a temporary-file error.

## PC builds (Windows and Linux)

Ignore this section if you just want to play on 3DS. The PC port is CTR Native beta 7.1 plus the changes listed above.

### Prerequisites

Windows:

1. Install [MSYS2](https://www.msys2.org/)
2. In an MSYS2 terminal:
   ```
   pacman -Syu
   pacman -S --needed git mingw-w64-i686-gcc mingw-w64-i686-cmake mingw-w64-i686-make
   ```
   If the update asks you to close the terminal, reopen MSYS2 and run the install command.
3. Add `C:\msys64\mingw32\bin` to your system PATH

Linux (Debian/Ubuntu):

```
sudo apt install gcc-multilib
sudo apt install libx11-dev libxext-dev libgl1-mesa-dev libasound2-dev libudev-dev libdbus-1-dev
```

SDL3 is compiled from vendored source, so nothing else needs installing.

### Building

```
build.bat            # Windows
chmod +x build.sh
./build.sh           # Linux
```

The first build compiles SDL3 and caches it as a static library in `build/`; later builds recompile only touched sources. For a clean rebuild, delete `build/` first.

Output: `build/ctr_native.exe` (Windows) or `build/ctr_native` (Linux).

### Running

A release build needs only the executable and your disc image beside it:

```
CTR-3D/
  ctr_native.exe
  assets/
    ctr-u.bin
```

For a development build run from `build/`, put `assets/` at the repository root, next to `build/`. The disc image rules are the same as for the 3DS.

Extracted asset files are supported for development and modding, and override the disc image when present: `assets/BIGFILE.BIG`, `assets/SOUNDS/KART.HWL`, `assets/TEST.STR`, `assets/XA/ENG.XNF`, and the `XA/ENG/EXTRA`, `XA/ENG/GAME` and `XA/MUSIC` streams.

Internal builds can record and replay input for bug reports; see [docs/REPLAYS.md](docs/REPLAYS.md).

## Architecture and documentation

```
main.c (entrypoint; one unity translation unit)
  +-- platform/native_*.c      native platform: audio, input, memcard, CD, renderer, PSX facade glue
  +-- platform/n3ds/           3DS backend: libctru/citro3d renderer, audio and render threads
  +-- game/game_unity.h
        +-- game/**            decompiled game source (CTR-ModSDK derived)
              +-- include/**   structs, globals, declarations, platform facade headers
```

The game code talks to the host only through the PlayStation library surface and a small platform contract; `CTR_NATIVE` marks native-side code. The 3DS renderer reimplements the PC renderer's contract against the PICA200's fixed-function GPU, which has neither fragment shaders nor an indexed-texture format, so palette lookup happens in a CPU-side decode cache.

- [docs/3DS_PORT.md](docs/3DS_PORT.md): how the 3DS port works, from threads to renderer to audio.
- [docs/MEMORY_MODEL.md](docs/MEMORY_MODEL.md), [docs/DATA_SECTIONS.md](docs/DATA_SECTIONS.md), [docs/OVERLAYS.md](docs/OVERLAYS.md): how the PS1-shaped memory model survives on a native host, inherited from CTR Native.

## License

GPL-3.0, inherited from CTR Native and CTR-ModSDK; see [LICENSE](LICENSE). Third-party components and their licenses are listed in [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md). No game data is included or distributed; you need your own copy of the game.

## Credits

- [zando95/CTR-3D](https://github.com/zando95/CTR-3D) — the 3DS port this fork is based on: the 3DS platform layer and renderer
- [CTR Native](https://github.com/CTR-tools/ctr-native) — the native PC port this project is a fork of
- [CTR-ModSDK](https://github.com/CTR-tools/CTR-ModSDK) — the decompilation project both are built on
- [ReservesMeter](https://github.com/CTR-tools/CTR-ModSDK/tree/main/mods/Modules/ReservesMeter) by Superstarxalien — the in-race boost-reserves meter's design (geometry, fill scale, colors) follows this community mod
- [PsyCross](https://github.com/OpenDriver2/PsyCross) — original PS1 compatibility code from which parts of the owned platform layer and PsyQ facade headers are derived
- [SDL3](https://github.com/libsdl-org/SDL) — cross-platform multimedia for the PC build
- [devkitPro](https://devkitpro.org/), libctru and citro3d — the 3DS homebrew toolchain and GPU library the 3DS port is built on
- Crash Team Racing is a trademark of Sony Computer Entertainment / Naughty Dog
