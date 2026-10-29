#ifndef CTR_FPS60_H
#define CTR_FPS60_H

/*
 * Runtime 60 fps game logic (game/MAIN/MainFps60.c, docs/3DS_PORT.md "Frame
 * pacing"). Retail CTR ticks its game logic once per displayed frame at 30 fps
 * (2 vblanks). When the platform shows frames for a single vblank, the frame's
 * logic runs in "60 Hz mode": g_fps60 is 1 for the whole frame (GameLogic and
 * RenderFrame), else 0 -- and with g_fps60 == 0 every patched site takes the
 * original expression, so 30 / 20 fps are the retail code paths.
 *
 * Two worlds, at 60:
 *   - 60 Hz world: the drivers' funcPtrs (PLAYER), ROBOT, CAMERA and TURBO
 *     buckets, and everything drawn every frame (HUD, water, cycle textures
 *     read through the 30 Hz timer, see below). Frame-counted logic there is
 *     patched per site with the macros below (ported from CTR-ModSDK's
 *     USE_60FPS patch; FPS_* have the same meaning as there).
 *   - 30 Hz world: every other thread bucket (weapons, hazards, HUD threads,
 *     ghosts, ...), particles, weather, AI item use, the ghost recorder. They
 *     tick on every other 60 Hz frame (the "full" frame) with gGT->timer and
 *     gGT->elapsedTimeMS swapped for 30 Hz values (Fps60_Enter30/Leave30), so
 *     their frame-counted code needs no patch at all. Values they write that
 *     the 60 Hz world counts down (driver timers) are doubled with FPS_DOUBLE
 *     at the write (g_fps60 is the frame's mode, also inside the 30 Hz world).
 */

extern int g_fps60;        /* this frame's logic runs at 60 Hz */
extern int g_fps60Full;    /* this frame also ticks the 30 Hz world (always 1 at 30) */
extern int g_fps60In30;    /* inside Fps60_Enter30/Leave30 (30 Hz world code) */
extern int g_fps60Allowed; /* the game context allows 60 (read by the frame pacer) */
extern int g_fps60ForceParticle; /* one-shot bursts from 60 Hz code: Particle_Init never thins them */

#define FPS60 (g_fps60 != 0)

/* CTR-ModSDK common.h USE_60FPS macros, runtime. */
#define FPS_DOUBLE(x)     (g_fps60 ? ((x) * 2) : (x))
#define FPS_HALF(x)       (g_fps60 ? ((x) / 2) : (x))
#define FPS_LEFTSHIFT(x)  ((x) - g_fps60)
#define FPS_RIGHTSHIFT(x) ((x) + g_fps60)

/* ModSDK `#ifdef USE_60FPS if (gGT->timer & 1) #endif STATEMENT`: the
 * statement runs every frame at 30, every other frame at 60. */
#define FPS60_HALF_RATE(timer) ((g_fps60 == 0) || (((timer) & 1) != 0))

/* The 30 Hz timer: gGT->timer minus the 60 Hz frames that did not tick the
 * 30 Hz world. Equal to gGT->timer while the game never ran at 60. Use for
 * anything that reads gGT->timer as "retail frames" from 60 Hz code. */
int Fps60_Timer30(void);

/* Swap gGT->timer / elapsedTimeMS / sdata->frameCounter to the 30 Hz
 * world's values (nestable). */
void Fps60_Enter30(void);
void Fps60_Leave30(void);

/* Main-loop hooks (MainMain / MainFrame_GameLogic / RenderSubmit). */
struct GameTracker;
void Fps60_BeginFrame(struct GameTracker *gGT); /* decide g_fps60 / g_fps60Full */
void Fps60_AfterTimerTick(struct GameTracker *gGT); /* after timer++ and elapsedTimeMS */
void Fps60_BeforeSubmit(struct GameTracker *gGT); /* g_fps60Allowed for the next flip */

/* Speed-check counters (cumulative; the [PACE] line prints deltas). A
 * "half tick" is half a retail frame: a 60 Hz logic frame adds 1, a 30 / 20
 * fps one 2. Frame-counted logic runs at full speed when halfTicks advances
 * by one per emulated vblank (retail at 20 fps: 2 per 3 vblanks). */
struct Fps60Stats
{
	unsigned logicFrames; /* MainFrame_GameLogic frames that ticked (not paused) */
	unsigned frames60;    /* ... of which at 60 Hz */
	unsigned halfTicks;
	unsigned ticks30;     /* 30 Hz world ticks */
	unsigned switches;    /* g_fps60 changes */
};
extern struct Fps60Stats g_fps60Stats;

#endif
