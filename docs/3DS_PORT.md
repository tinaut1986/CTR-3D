# The 3DS port

How CTR 3D runs Crash Team Racing on a New Nintendo 3DS. This describes the
shipped design. The PS1-shaped memory model the game keeps on every platform is
covered by `MEMORY_MODEL.md`, `DATA_SECTIONS.md` and `OVERLAYS.md`.

## Layout

- The game logic is the CTR-ModSDK decompilation. It reaches the host only
  through the PlayStation library surface (`libgpu`, `libspu`, `libpad`, ...),
  the small `Platform_*` contract in `include/platform.h`, and a set of named
  `Native*` hooks. There are no platform `#ifdef`s inside `game/`.
- `main.c` and every `platform/native_*.c` file form one unity translation
  unit; `game/game_unity.h` includes all of `game/` into it in dependency order.
- `platform/n3ds/n3ds_backend.c` is the one separate translation unit, so that
  libctru's headers never meet the decompilation's own. The game's struct
  layouts assume 4-byte enums, so the port is compiled with `-fno-short-enums`;
  the backend is compiled with `-fshort-enums` to match libctru's ABI. The two
  sides talk through plain scalars declared in `n3ds_backend.h`.
- `platform/n3ds/sdl3_compat_3ds.c` provides the few SDL entry points the shared
  platform code calls, so `platform/` stays source-compatible with the PC build.

## Threads and cores

- The game runs on the main thread, on core 0.
- A render worker on core 2 walks the ordering table and drives the GPU, so the
  game's next frame overlaps the current frame's draw. If the core is refused,
  drawing runs on the main thread.
- An audio worker on core 3 (core 2 as a fallback, then the main thread) feeds
  the DSP.
- Core 1 belongs to the system. The port never creates a thread there and never
  raises the application CPU time limit; either one hangs the console when the
  Rosalina overlay opens.
- Anything the game waits on that a host audio callback would normally advance
  (the SPU/XA sample clock) is driven from the vblank instead
  (`NativeAudio_SetDeterministicRenderMode(1)`), because nothing on the 3DS
  consumes audio at a rate the game can block on.

## Renderer

The PICA200 has no fragment shaders and no indexed-texture format, so the PC
renderer's per-pixel palette decode cannot be reproduced. The 3DS renderer
(`platform/n3ds/native_renderer_3ds.c`) implements the same contract
(`include/platform/native_renderer.h`) differently:

- **Texture decode cache.** PS1 VRAM is decoded on the CPU into RGBA textures
  in sub-page cells, keyed by page, palette and the freshness of the VRAM rows
  each cell covers. A per-palette lookup table converts a cell in one pass.
- **Track.** Each level's track is baked into a resident mesh at load time and
  drawn by the GPU with a true perspective projection
  (`shaders/ctr_track.v.pica`), with a depth range fitted per BSP leaf. There is
  no PS1 vertex snapping or affine texture warping on the road. Animated
  textures and vertex animation (water, turbo pads) patch the resident mesh.
- **Models.** Karts, drivers and instances draw through a native model path on
  the GPU (`shaders/ctr_model.v.pica`) with the same projection.
- **Everything else.** UI, particles, effects and any remaining PS1 primitive go
  through the packet path: the game's ordering table is walked and converted to
  GPU triangles.
- **Depth.** The PS1 sorts by draw order; the 3DS uses a depth test. Every
  emitted primitive carries a depth resolved through `PacketDepth_ResolveXY`:
  GTE-derived depth where the geometry has it, otherwise its emit ordinal, so
  retail draw order survives as depth. Ground decals (shadows, skid marks, tire
  contact) get fixed depth brackets so they neither sink nor float. The UI is
  drawn off the depth test, in order. An emit path that skips the resolver draws
  through walls; the `[PKTDEPTH] escaped` counter must stay zero.
- **Draw merge.** Opaque primitives are binned by GPU state across the walk and
  submitted in a handful of draws; blended primitives are replayed in order. A
  frame is around twenty draws.
- **Transparency.** Semi-transparent surfaces draw twice: once opaque with depth
  writes, once blended without.
- **Widescreen.** The top screen renders a wider field of view. Widening the
  orthographic window the game projects through is widening the field of view;
  the HUD ramps out to the new edges.
- **Stereoscopic 3D.** The frame's draw commands are journaled and the right eye
  replays the journal through a second projection. The slider sets the
  separation frame by frame; the UI sits on the screen plane with a small
  pop-out. The replay re-issues draws from the vertices the left eye already
  wrote (nothing is re-converted or re-appended), so its CPU cost is the
  citro3d draw issue of the frame's ~20-300 draws; the left eye pays nothing
  extra but the journal entries. OPTIONS > 3D EFFECT is ON / OFF, plus AUTO on
  an Old 3DS (see "Old 3DS / 2DS").
- **Bottom screen.** The map's primitives are marked in the UI stream, peeled
  off at conversion time, and replayed on a bottom-screen target through an
  orthographic window fitted to their bounds. In map mode a copy stays on the
  top screen as well. The frame-rate readout is drawn in the game's own font
  inside the same bracket.
- **Not rendered.** The close-up "mosaic" texture tier, and retail's full-screen
  feedback blur (the clock item's screen effect and the intro flyby's ghost
  trail). Texture filtering is an option, off by default.

Rules the hardware enforces and emulators do not:

- Write every component of every declared shader output. A vertex is emitted
  only once all components of all `.out` registers are written; a
  partially-written output stalls the shader unit and the next frame begin
  waits forever on a blank screen.
- Vertex-shader float uniforms are physical registers shared across every
  shader program. Declare `projection[4]` first in every program, or re-assert
  it after a state bind.
- `mul` cannot take a uniform or constant as its second operand; swap the
  operand order.
- Route every geometry emit path through `PacketDepth_ResolveXY`.
- Azahar runs the CPU at real New 3DS speed but hides all of the above, and its
  frame timing is not the console's. A change that only ran in the emulator is
  unproven.

## Audio

- The SPU's voices play as hardware voices on DSP channels 0 to 22 through
  ndsp. Channel 23 carries the XA music stream at its native sample rate, or
  the whole software mix when the hardware voices are unavailable. Software
  mixing on the ARM11 is not fast enough to be the primary path.
- XA music streams from the disc image on the SD card. The XA voice lines are
  cached in RAM after boot, in the idle part of each frame (the frame pacer's
  wait, never during a level load), 8 sectors per step; a line played before
  its track has landed streams from the card. The cache used to fill while
  the Sony logo was held on screen, which made the boot ~16 s. No cache on
  an Old 3DS (the heap has no room for it).
- Output gain is twice retail's master level, because the PS1's master volume
  of 0x3fff is half of digital full scale.

## Disc, saves and memory

- The game reads `assets/ctr-u.bin` from the first of `sdmc:/3ds/ctr_native`,
  `sdmc:/3ds/ctr-native`, `sdmc:/ctr-native` and `sdmc:/` that contains it, and
  changes directory there. Saves live in `memcards/` beside it. A read cache and
  prefetch cover the XA streams and level loads.
- The heap keeps the PS1's 2 MB MEMPACK arena size on purpose: it flags any
  change that needs more memory than the original had. Memory is not the
  constraint on this hardware; CPU time is.

## Debug surface and measuring

- `CTR_INTERNAL` (CMake option, default ON; the release is built with it OFF)
  compiles the debug surface: the dumpreq words (`stereo`, `drawmerge`, `sdlog`,
  ...) sent with `SITE DUMPREQ <word>` over FTP or from the DEBUG tab (there is no
  file channel: polling `sdmc:/dumpreq.txt` stalled the frame for up to 230 ms
  when the SD card was busy), SELECT chords, `[PERF]` scope windows, screenshots and probes.
  OFF strips all of it from the binary and changes no default.
- Bottom screen (`platform/n3ds/n3ds_bottomui.c`, always on, from the first
  frame): graphics mode, no libctru console on it. Tabs ESTADO / MAPA /
  OPCIONES / ACTUALIZADOR (English on non-Spanish consoles: STATUS / MAP /
  OPTIONS / UPDATER), plus one DEBUG tab (PERF sparkline + memory, ACTIONS = the
  dumpreq buttons and the FTP toggle, LOG) with `CTR_INTERNAL`. There is no
  Achievements tab: the port has no achievement system. Why one owner matters:
  `consoleInit(GFX_BOTTOM)` silently sets RGB565 + single buffering, so the old
  console, a 24-bit CPU painter and the minimap's display transfer were three
  writers in two formats on one buffer. Now `N3DS_GfxInit` fixes BGR8 + double
  buffering once, and from the first GPU frame the citro3d bottom target is the
  only presenter: the UI is a CPU-drawn 320x240 picture uploaded as a 512x256
  RGBA5551 texture and drawn as one quad; the minimap (MAPA tab) is drawn over
  it in the content area under the tab bar. Before the first GPU frame the same
  picture is blitted on the CPU. It repaints only on change; idle it costs nothing.
  Log text goes to the DEBUG > LOG view, the SD log and the 3dslink stream.
  Settings (ftp, stereo, fpsmode, cpu804, hudhide, showfps) persist in `ctr3d.cfg` in the app dir.
  OPTIONS holds AUTO-HIDE HUD, 3D EFFECT, FRAME SKIP, SHOW FPS and (New 3DS only) CPU 804MHZ; the
  SD log switch is in DEBUG > ACTIONS. The map and the race HUD are always on this screen ("Race HUD
  on the bottom screen"); `btmmap` survives only as a dumpreq word and is no longer saved.
- App data lives in `sdmc:/3ds/ctr_native/` (assets/, memcards/, update/,
  ctr3d.cfg, enable-log.txt, dev-ftp.txt) for a .3dsx and an installed CIA alike.
- FTP (`n3ds_ftp.c`, port 5000, one lowest-priority thread on core 3, else 2;
  on an Old 3DS / 2DS core 0 behind the game, see "Old 3DS / 2DS"):
  `CTR_INTERNAL` builds serve the whole SD card by default (toggle in DEBUG >
  ACTIONS); production builds serve nothing unless `dev-ftp.txt` exists, then only
  `/3ds/ctr_native/update` and `SITE INSTALL` (CANCEL on the Updater tab).
  `./build3ds.sh --install <ip>` uploads and installs the CIA; the game warns on
  the bottom screen (upload %, install %, 10 s countdown) and restarts into it.
- CIA: `./build3ds.sh` builds the .3dsx and `build-3ds/cia/ctr3d.cia`
  (`package-cia.sh`; title ID 0x0004000005A2C300). One CIA for both consoles, as
  the author's other ports: SystemModeExt 124MB on a New 3DS (178MB made the return to HOME much slower), SystemMode 80MB on
  an Old 3DS / 2DS (exheader ARM11 caps +0xD low nibble = 2, +0xE high nibble = 3;
  96MB crashed the 2DS HOME menu in the other ports). See "Old 3DS / 2DS" below.
- In any build, an empty `enable-log.txt` in `sdmc:/3ds/ctr_native/` turns on the SD log
  at `sdmc:/ctr3ds.log`. It is flushed on a timer, because a flush per line
  stalls the game on the card's latency. The flush runs on a low-priority writer
  thread (lines go to a 64 KiB RAM buffer; dumpreq `logasync` toggles the old
  flush on the logging thread): on an Old 3DS the 2 s fflush+fsync was a
  100-285 ms frame.
- Load diagnosis: `[LOADPROF]` lines (debug builds) log any load step that holds the game thread for
  250 ms or more: `stage=` (one `LOAD_TenStages` call), `queue stage=` / `file type= idx= size=` (one
  queued file, `LOAD_NextQueuedFile`), `cdread idx=` (the read) and `ptrmap idx=` / `dramcb idx=` (the
  pointer patch and the file's callback). A load shows no animation while one of these runs, because
  the game draws one frame per call.
- `Platform_Log` does not reach `ctr3ds.log` on the 3DS; use `N3DS_DebugLog` (or `Platform_LogInstrument`)
  for anything meant to be read from the SD log.
- `SITE SHOT` writes `debug/top.bmp` and `debug/bottom.bmp`; the bottom one is the real framebuffer
  when the GPU draws that screen (the CPU snapshot only knows the UI layer, not the map or HUD). The
  command answers 451 while the screen is slow, but the files still appear a few seconds later.
- Every FTP request (state, shot, log read) stalls the Old-3DS game thread for 1-2 s: do not poll the
  console while measuring a load or a race. `SITE INSTALL` restarts the game, so it ends whatever the
  player was doing; the restart is driven by the main loop, so a game stuck in a load restarts only
  when it gets out of it (closing from HOME and reopening starts the installed build at once).
- Frame-rate claims count only from a wall clock on hardware: the `[PERF]`
  windows in the SD log, or the on-screen readout.

## Old 3DS / 2DS (memory fits in 80 MB; CPU is the open part)

Memory, measured from the code (`n3ds_heap.c` has the table): ~18 MiB of
code+bss+stacks, then linear and heap. A New 3DS (124MB) keeps the full split
(linear 88 MiB, heap ~71 MiB, 11 decode atlases, 3 MiB hw-voice store, 32 MiB XA
voice cache). A legacy region gets the lean split: linear 32 MiB holds the
~28.4 MiB Old-3DS linear budget (7 decode atlases = 14 MiB, 2 MiB hw-voice store,
4 MiB track atlas, ~4 MiB track geometry, ~3.5 MiB gfx/citro3d/vertex buffer),
the heap the rest: ~29.5 MiB at 80 MB, ~21.5 MiB at 72 MB, ~15.5 MiB at 64 MB
(below the ~15 MiB loading peak plus margin: 64 MB is not expected to work). The
decode cache keeps its 96 full-page cells at 7 atlases; only the sub-page tier
shrinks (208 slots against a measured peak of ~86). The voice cache sizes itself
to the heap and stays on the SD path when it does not fit. A 2DS gets no
right-eye render target (VRAM) and no 3D mode; that saves no linear memory (the
top framebuffers are always allocated at the stereo width).

An allocation the game cannot run without fails loudly: citro3d / atlases at boot
(N3DS_ShowFatalGfxScreen), the track atlas or track geometry at level load
(N3DS_FatalMemRequest: the main loop shows the numbers on the top screen and
quits after START). `[MEM]` lines: `gfxinit`, `atlases`, `trackatlas`,
`trackgeom +N KiB`, `voicecache-*`, `window` (periodic), `level-end #N`
(the previous level's heap extent peak and linear low-water mark) and `fatal`.

CPU: 268 MHz without L2 against ~9-11 ms main thread + 3-7 ms render worker per
frame at 804 MHz, and the render worker (core 2) and audio worker (core 3) do not
exist, so they fall back to synchronous work on core 0. The FTP server and the
updater run on core 0 at priority 0x3F: the kernel schedules a core strictly by
priority, so they only get the time the main thread (0x30) spends blocked on the
GPU or the vblank. FTP polls with a zero timeout and naps 50 ms idle / 10 ms in a
transfer (1 s without Wi-Fi). Core 1 is never used (Rosalina hang; see
`n3ds_audio_thread.c`).

Old-3DS simulation, two ways (both log `[O3DS-SIM]` lines; `[MEM]` lines report
heap/linear use, grep them for the real peak per level):
- DEBUG > ACTIONS > OLD 3DS SIM, or an `o3ds-sim.txt` in `sdmc:/3ds/ctr_native/`
  (optional words: `mem=80` legacy region in MiB, `ftp=0`). At start it turns the
  clock-up and L2 off (needs `ptm:sysm`, logged), makes the render and audio
  workers unavailable as on a real Old 3DS, puts FTP and the updater on core 0 as
  there, and parks ballast so heap and linear match a legacy region of `mem` MiB
  (the atlas count and the hw-voice store follow the simulated linear heap). The button applies the clock at once; memory
  and workers need a restart.
- With no workers, audio runs the worker's own per-vblank job on the main
  thread (`N3DS_AudioRunVBlankOnCaller`: event-batched voice lifecycle, the
  hardware-voice mirror, XA parcels at the source rate); the `audioprof` line
  reports its split (`vbl` counts vblanks, `wakes` stays 0). The OT walk's
  cost per node class is the `[PARSEPROF]` line (268 MHz ticks, i.e. Old-3DS
  cycles); dumpreq `g4run` A/Bs the menu flag's fast path. Inside that run the flag's quads
  skip the depth resolve and the float vertex conversion (dumpreq `g4fast`, default on):
  they share the first quad's resolved occlusion stamp and read only their own
  parallax slot. `g4check` emits every 8th quad both ways and reports `[G4CHECK]
  checked/bad`; `[G4PROF]` is the sampled cycles per quad (~3970 -> ~1470 on an Old 2DS).
- The game thread's own CPU work: `[INSTPROF]` splits
  `mainframe_execute_instances_ms` per draw-func kind (`normal`, `alt`, `split`,
  `special`, `refl`: drawn instances per frame, whole cost, cycles per instance)
  and, for DrawFunc_Normal, counts commands, decoded/cached vertices, RTPT/RTPS
  projections, accepted, claimed (native model path) and packet triangles, with
  the sampled cycles and frame estimate of each phase (`dec` vertex decode,
  `proj` projection + cull gate, `cap` native-model capture, `pkt` packet
  write). `[GTEPROF]` counts software-GTE ops per frame per phase (`logic`,
  `queue`, `exec`, `level`, `effects`, `ui`, `other`) with the six most
  frequent. Both are CTR_INTERNAL-only, one line set per `[PERF]` window.
- Native-model gate (`native_model_gate.c`, dumpreq `mdlgate`, default on): an
  instance the model path replaces is transformed by the GPU, so its triangles
  no longer go through the software GTE just to be accepted. The gate keeps the
  part of RTPS/RTPT the accept test reads (the SXY/SZ FIFO and FLAG bit 18) in
  the GTE's own integer arithmetic and runs NCLIP and the screen window on it:
  the same triangles, no capture Stores, no NCLIP/AVSZ3 ops. A triangle the
  capture declines replays the retail projection from the saved FIFO.
  `[INSTPROF]` `gate=` counts gated projections; `proj` is their cost.
  The menu flag's 360 RTPT a frame take the same arithmetic
  (`NativeModelGate_RtptStore`, dumpreq `flaglean`, default on): same SXY and
  the same capture Stores, without the op emulation. With `flagpar` (default
  on) the flag makes no Stores at all: each quad hands the walk its four
  remapped stereo depths through `GTECapture_FlagParallaxTable`, so the walk's
  ~4000 parallax lookups per frame become reads of the quad's own slot (same
  values as a capture hit, no misses or evictions).
- The software GTE (`native_gte_core.c`) issues each constant op word through
  its own specialised copy (`GTE_Op*`, picked by the always-inlined doCOP2) and
  inlines the register moves; the arithmetic is PsyCross's, bit for bit.
  `platform/n3ds/tests/perf-parity/run.sh [BASE_REV]` checks that on the host:
  the GTE of a base revision vs the working tree on random register files, and
  RenderBucket_Execute on random model scenes (prim memory, OT, scratchpad, GTE
  registers, capture table, native-model capture calls). Run it before
  committing a change to either.

Frame pacing (OPTIONS > FRAME SKIP, cfg `fpsmode`: AUTO / 30 / OFF; the old
FRAME CAP `fpscap` is migrated: AUTO and 30 keep, 20 becomes AUTO). The game flips
after `vsyncTillFlip` emulated vblanks, set at each submit from
`Platform_FlipVBlanks`: 1 (60 fps), 2 (30 fps, retail) or 3 (20 fps). The policy is
`platform/n3ds/n3ds_framepace.c` (host test: `platform/n3ds/tests/run-fps60-tests.py`):
- AUTO (default, every console): 60 while frames fit, one step down (30, then 20)
  when 8 of 32 frames miss the current interval, one step up after two 32-frame
  windows with at most one frame over the faster interval minus a margin (4 ms for
  20 -> 30, 3 ms for 30 -> 60, i.e. 13.7 ms of work). Two windows of dwell after
  each change; a 60 that misses drops at once, and if it held less than 8 windows
  the next 30 -> 60 waits twice as long (2, 4, ... 64 windows, reset after 32
  windows at 60), so a scene on the edge settles at 30 instead of flickering. An
  Old 3DS normally sits at 30 or 20 as before.
- OFF: 1 vblank whenever the context allows it; a frame that does not fit simply
  takes the vblanks it needs.
- 30: always 2 vblanks, the retail cadence and code paths.
A frame is only shown for 1 vblank where the game allows 60 (`g_fps60Allowed`,
`game/MAIN/MainFps60.c`): racing, battle, time trial and the adventure hub's
driving. Main menu, pause, any open menu, cutscenes / podium / credits, end of
race, demo mode and loads (the checkered flag) stay at 30.

60 fps game logic (`include/ctr_fps60.h`, `game/MAIN/MainFps60.c`): the frame after
one shown for 1 vblank runs its logic in 60 Hz mode (`g_fps60`, decided per frame
from `Platform_LastFrameVBlanks`; a late frame is paid back by the next one ticking
a whole retail frame, so the sum stays right). Retail logic is partly time based
(`elapsedTimeMS` from the root counter, clamped at 64 ms: correct at 60, 30 and 20)
and partly frame counted; the frame-counted part is handled in two worlds:
- 60 Hz world, ticked every frame and patched per site with runtime ports of
  CTR-ModSDK's `USE_60FPS` patch (`FPS_DOUBLE`/`FPS_HALF`/`FPS_RIGHTSHIFT`/
  `FPS60_HALF_RATE`, each site commented `// 60fps:`): the drivers' funcPtrs
  (`game/Vehicle/*`), AI driving (ROBOT, `BOTS.c`), the camera (`CAM.c`), turbo
  flames, and everything drawn every frame (HUD in `game/UI/*`, fades, the hub
  map, lap stats). Model animation data is NOT doubled as in ModSDK (not
  runtime-switchable): driver animation frames advance on 30 Hz ticks.
- 30 Hz world, ticked on every other 60 Hz frame with `gGT->timer`,
  `elapsedTimeMS` and `sdata->frameCounter` swapped to 30 Hz values
  (`Fps60_Enter30`/`Leave30`), so its retail code needs no patch: every other
  thread bucket (weapons, hazards, crates, HUD threads, Aku hints, ghosts),
  particles (`Particle_Init` drops 60 Hz-world spawns on half the frames, one-shot
  bursts set `g_fps60ForceParticle`), the ghost recorder, AI item use
  (`PickupBots_Update`), Roo's Tubes bubbles, rain motion, the clock weapon
  countdown. Values it writes that the 60 Hz world counts (weapon roulette,
  HUD fly-ins) are `FPS_DOUBLE`d at the write (`game/231`). Texture cycles and
  water / SCVert read the 30 Hz timer (`Fps60_Timer30`).
With the game never at 60, `g_fps60` stays 0, every frame is a 30 Hz tick and
nothing is swapped: the retail paths bit for bit. HOWL runs per vblank, so the rate
changes neither music tempo nor sound timers. The emulated vblank is 60.00 Hz and
the panel ~59.83 Hz, so one frame in ~115 at a steady cap still shows a vblank
short. Expect at 60: weapons, hazards, particles, ghosts and anything a 30 Hz
thread places on the kart (Aku mask, shields, TNT on the head) move at 30.

3D on an Old 3DS: 3D EFFECT defaults to AUTO there (a saved choice wins). The
right eye is dropped while it alone makes the frame miss 50 ms (or the forced 30
cap's 33.3 ms) in 8 of 32 frames, and comes back when the frame plus its last
measured right-eye cost fits with 3 ms to spare; 3D at 20 is preferred to 2D at
30. The status tab says "2D (AUTO, SLOW)" meanwhile. A 2DS / New 2DS XL has no
3D panel: no right-eye target and gfxSet3D(false), whatever the option says.

Measuring (CTR_INTERNAL, SD log, every 64 frames after `[GPUTIME]`):
- `[ENDPROF]`: platform_end_scene_ms split into contiguous phases, avg us per
  frame: `sky trk mrg ui mdl smk oth rEye btm fend` (their `sum` is the whole
  EndScene), `flush` (the range flush of `c3dflush 1`), `begin` (latch + wait for
  the previous GPU frame + clears; ~0 on the no-worker path, where the GPU's
  3-7 ms overlap the next frame's game logic), `rEyeSt` (right eye per stereo
  frame) and the journal per stereo frame (`pkt vram leaf run inst brk ovf`).
- `[PACE]`: `cap` in force, `mode` (auto / 30 / off), `avgWorkUs`/`maxWorkUs`
  (flip to flip minus the pacer's sleep), `miss` (frames over the cap), `changes`,
  `3d` (AUTO shows `(2D)` when it has the right eye off), `rEyeUs`; game speed:
  `logic` (game frames that ticked), `at60` (of which in 60 Hz mode), `speed`
  (frame-counted logic speed in % of retail: half ticks per emulated vblank; 100 at
  a steady 60 or 30, 67 at 20 as on retail; paused or loading windows read low),
  `t30` (30 Hz-world ticks, ~vbl/2 at full speed), `vbl`, `sw` (60 <-> 30 logic
  switches), `up60` (AUTO's current 30 -> 60 backoff in windows); plus one
  `[PACE] auto ...` line per decision.
- `[FPS60] lap N`: per lap of player 1, the race clock and the real (vblank) time
  of the lap, logic frames and how many at 60, `speed`, and the top kart speed
  (`speedApprox`). The same driving at 30 and at 60 should give the same lap times
  and top speed.
- dumpreq `c3dflush [N]`: 0 (default) lets C3D_FrameEnd flush the whole linear
  heap every frame, 1 flushes only the frame's vertex range (GX_CMDLIST_FLUSH for
  the command list). A/B with `fend` on hardware before changing the default.
- `./build3ds.sh --o3ds-sim` also builds `build-3ds/cia/ctr3d-o3dssim.cia` (own
  title ID 0x0004000005A2C400, SystemModeExt Legacy, SystemMode `O3DS_MEM`
  default 80MB, 268MHz, no L2). It installs next to the normal CIA and shares the
  SD folder. The game detects the small region and simulates the rest.
- Background voice-cache fill (after boot, in the pacer's vblank wait): stops
  reading once the budget is full (a legacy region's ~5 MiB fills from the first
  third of GAME; dumpreq `vcstop`), and on the Old-3DS class a step starts only
  when the worst recent step plus 2 ms fits before the vblank target (dumpreq
  `vcpace`). `[VFILL]` (windows with fill activity): `steps`, `avgUs`, `maxUs`,
  `overruns` (steps that ended past the target: a late frame), `deferred`,
  `windowUs`. `[XAPRELOAD] budget misses=... full=... filesSkipped=...` at the end.
- `[LVLPROF]`: mainframe_level_geometry_ms by phase (1P): `anim`
  (AnimateWater/AnimateQuad), `bsp` (render-list walk), `rec`
  (NativeTrackFrame_Record), `sky` (DrawSky_Full + glow).
- `[TRKANIM] window ... animPatched/f`: animated track vertices (water) patched
  per walk; only those in the leaves the walk draws (dumpreq `trkanimvis`).
- Bottom UI: the STATUS / DEBUG live repaint ticks every 1 s on the Old-3DS class
  (250 ms on a New 3DS).

### Level loads (Old 3DS)

A race load took ~27 s and the demo scene ~40 s with a frozen screen. `[LOADPROF]` showed it was not the
disc (`cdread` ~80 ms) but `LOAD_RunPtrMap`: in `CTR_INTERNAL` builds each patched pointer called
`NativeCheckpoint_RegisterPointerSlot`, which scanned its whole list (up to 65536) for duplicates. It now only
appends; the list is sorted and deduplicated when a checkpoint is saved, and once when it fills. Loads dropped
to 0.3-0.5 s a file (the level 40 s -> 0.5 s). Production builds never had the cost. The
`N3DS_BeatMainTick` memory sample (`linearSpaceFree()` stalled 0.1-1.2 s every 512 calls) is gone for the same
reason; the water marks are sampled when a level begins.

What is left is the frame count: the game still draws one frame per `LOAD_TenStages` call, so the loading flag
pauses briefly around the longest calls and the boot stage 0 (~8.9 s) shows nothing. An experiment that
presents a flag-only frame from inside the long steps (`perf/loading-frame-pump`) crashed the first time (a
full `MainFrame_RenderFrame` re-entered menu input); the reduced version was not verified. Risk: it draws while
a file is half read.

## Race HUD on the bottom screen

In a 1P race the HUD is drawn on the bottom screen around the map, and the top screen drops it when
OPTIONS > AUTO-HIDE HUD is on and the MAPA tab is up (otherwise the top keeps its own copy).

- Marking: `UI_RenderFrame.c` wraps each HUD draw call in `HUD_GROUP(group, call)`, which records the
  `primMem` span it wrote (`NativeGpu_MarkUiHudRange`, up to 24 spans a frame, 7 groups: lap, clock,
  wumpa count, speedometer/slide/jump meters, held item, ranked-driver list, rank suffix). The two 3D
  pieces (the big rank number `instBigNum`, the fruit `instFruitDisp`) are `Birth2D` instances on the UI
  pushbuffer drawn through the retail packet path (`g_dbg_hudModelsAsUi`), so `RenderBucket_Execute`
  marks the packets each writes. 1 player only: other counts are not marked.
- Stamps: the resolver (`PacketDepth_ResolveXY`) takes marked nodes down the 2D UI leg and stamps them
  `PKTDEPTH_UI_STAMP_BTMHUD_BASE + group` (0xFFF1..0xFFF7) while the ordered-UI pin owns the band.
- Renderer (`native_renderer_3ds.c`): `N3DS_BtmHudAppend` peels them into one list, each segment tagged with
  its group, plus a latched grow-only source box per group (reset after 90 frames without the group).
  `N3DS_DrawBottomMapPass` draws each group through its own ortho window (`N3DS_BtmFitWindow`) into the rect
  `N3DS_BtmHudLayout` gives it; unless AUTO-HIDE is on with the MAPA tab up, a pinned copy goes to the
  ordinary ordered-UI list for the top screen.
- Layout (`N3DS_BtmHudLayout`, panel content area 320 x 212 px): every group at one common scale, each in
  the corner nearest to its retail position (display centre decides left/right and top/bottom): clock top
  left, lap and fruit top right, speedometer bottom right, map in the middle. The driver list goes bottom
  left, directly below the top-left stack, and the player's rank number below it, filling the room left
  (at least 24 px). Fruit 0.9 of the common scale; the rank number 0.7 when there is no list. PSX units are
  anamorphic, so the two axes use the display's own ratio (same as the map).
- The FPS text is no longer part of this: OPTIONS > SHOW FPS draws it on the top screen, top centre.
- Not done: the held item is marked (group 6) but was never seen working (no test race picked up a box);
  battle, relic race and time-trial specifics; 2-4 players; the adventure hub's own HUD
  (`UI_RenderFrame.c` second function).

## Build options

| Option | 3DS default | Effect |
|---|---|---|
| `CTR_INTERNAL` | ON (release: OFF) | The debug surface above |
| `CTR_NATIVE_TRACK_RENDERER`, `CTR_NATIVE_MODEL_RENDERER`, `CTR_NATIVE_GTE_CAPTURE` | ON | The resident track and model paths and the sub-pixel capture table stereo is built on; the OFF legs do not link on 3DS |
| `CTR_NATIVE_DIRECT_EMIT` | ON | Direct packet-to-bin emit for all-opaque ordering-table nodes |
| `CTR_NATIVE_FEEDBACK_BLUR` | OFF | Retail's full-screen feedback blur; PC keeps it ON |
| `CTR_NATIVE_AUDIO_DECODE_PROFILE`, `CTR_NATIVE_DISC_IO_PROFILE`, `CTR_NATIVE_OTWALK_PROFILE`, `CTR_NATIVE_PRIMMEM_WATERMARK` | OFF | Counters and log lines for profiling builds |
