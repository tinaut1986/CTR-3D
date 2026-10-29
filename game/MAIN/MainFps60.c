#include <common.h>
#if defined(CTR_NATIVE) && defined(CTR_INTERNAL)
#include <platform/native_log.h>
#endif

/*
 * Runtime 60 fps game logic: the frame's mode and the 30 Hz world's clock.
 * See include/ctr_fps60.h for the design and docs/3DS_PORT.md "Frame pacing".
 *
 * Not retail code. With the game never at 60 (the 30 fps option, the PC
 * build) g_fps60 stays 0, every frame is "full", Enter30/Leave30 swap
 * nothing and Fps60_Timer30() == gGT->timer: the retail paths, bit for bit.
 */

int g_fps60 = 0;
int g_fps60Full = 1;
int g_fps60In30 = 0;
int g_fps60Allowed = 0;
int g_fps60ForceParticle = 0;

// 60 Hz frames that did not tick the 30 Hz world: gGT->timer - s_fps60Lag
// is the 30 Hz world's frame counter.
static int s_fps60Lag = 0;
// The next 60 Hz frame is the in-between one (no 30 Hz tick).
static int s_fps60HalfNext = 0;
// elapsedTimeMS since the 30 Hz world last ticked, and the value it sees.
static int s_fps60ElapsedAcc = 0;
static int s_fps60Elapsed30 = 0x20;

#define FPS60_NEST_MAX 4
static int s_fps60SavedTimer[FPS60_NEST_MAX];
static int s_fps60SavedElapsed[FPS60_NEST_MAX];
static int s_fps60Swapped[FPS60_NEST_MAX];
static int s_fps60SavedFrameCounter[FPS60_NEST_MAX];

#ifdef CTR_NATIVE
// Speed check counters, read by the platform's [PACE] line.
struct Fps60Stats g_fps60Stats;
#endif

// Contexts that stay at 30 whatever the option: everything that only runs
// there (menus, pause, cutscenes, podium, end of race, loads) keeps the
// retail frame-counted code without a 60 Hz patch.
static int Fps60_ContextAllowed(struct GameTracker *gGT)
{
	if (gGT == NULL)
	{
		return 0;
	}

	if ((gGT->gameMode1 & (PAUSE_ALL | MAIN_MENU | GAME_CUTSCENE | END_OF_RACE | LOADING)) != 0)
	{
		return 0;
	}

	if (sdata->Loading.stage != LOAD_IDLE)
	{
		return 0;
	}

	if ((gGT->renderFlags & RENDER_FLAG_CHECKERED_FLAG) != 0)
	{
		return 0;
	}

	if (gGT->boolDemoMode != 0)
	{
		return 0;
	}

	// Cutscene levels, the adventure character select, credits, podiums.
	if ((gGT->levelID >= INTRO_RACE_TODAY) || (gGT->levelID == ADVENTURE_GARAGE) || (gGT->levelID == MAIN_MENU_LEVEL))
	{
		return 0;
	}

	if (LOAD_IsOpen_Podiums() != 0)
	{
		return 0;
	}

	// A dialog is open in the adventure hub (Aku/Uka hints): 30 Hz world UI.
	if (sdata->ptrActiveMenu != 0)
	{
		return 0;
	}

	return 1;
}

void Fps60_BeginFrame(struct GameTracker *gGT)
{
	int prev = g_fps60;

	g_fps60 = 0;
#ifdef CTR_NATIVE
	// The flip interval the last frame actually took: 1 vblank -> this
	// frame's logic is half a retail tick. Predicting from the last frame
	// keeps the sum right: a frame that runs long is paid back by the next
	// one ticking a full retail frame (see Platform_LastFrameVBlanks).
	if (g_fps60Allowed && Fps60_ContextAllowed(gGT) && (Platform_LastFrameVBlanks() == 1))
	{
		g_fps60 = 1;
	}
#endif

	if (g_fps60 == 0)
	{
		g_fps60Full = 1;
		s_fps60HalfNext = 0;
	}
	else
	{
		g_fps60Full = (s_fps60HalfNext == 0);
		s_fps60HalfNext ^= 1;
	}

#ifdef CTR_NATIVE
	if (prev != g_fps60)
	{
		g_fps60Stats.switches++;
	}
#else
	(void)prev;
#endif
}

#if defined(CTR_NATIVE) && defined(CTR_INTERNAL)
/*
 * [FPS60] lap line (SD log, once per lap of player 1): the race clock (time
 * based, so it matches real time at any rate unless frames ran over 64 ms),
 * the real time from the emulated vblanks, how much of the lap ran at 60 and
 * the frame-counted speed (half ticks per vblank, 100% = retail speed), plus
 * the top kart speed. Same driving at 30 and at 60 should give the same lap
 * times and the same top speed; a 60 Hz physics bug shows up as a different
 * top speed or consistently faster/slower laps.
 */
static void Fps60_LapLog(struct GameTracker *gGT)
{
	static int s_lap = -1;
	static int s_lapVbl;
	static int s_lapClock;
	static int s_lapTopSpeed;
	static struct Fps60Stats s_lapStats;
	struct Driver *d = gGT->drivers[0];
	const int vbl = Platform_GetVBlankCount();

	if ((d == NULL) || (gGT->elapsedEventTime < s_lapClock) || (s_lap < 0) || ((int)d->lapIndex < s_lap))
	{
		s_lap = (d != NULL) ? d->lapIndex : -1;
		s_lapVbl = vbl;
		s_lapClock = gGT->elapsedEventTime;
		s_lapTopSpeed = 0;
		s_lapStats = g_fps60Stats;
		return;
	}

	if (d->speedApprox > s_lapTopSpeed)
	{
		s_lapTopSpeed = d->speedApprox;
	}

	if ((int)d->lapIndex != s_lap)
	{
		const unsigned frames = g_fps60Stats.logicFrames - s_lapStats.logicFrames;
		const unsigned at60 = g_fps60Stats.frames60 - s_lapStats.frames60;
		const unsigned half = g_fps60Stats.halfTicks - s_lapStats.halfTicks;
		const int dv = vbl - s_lapVbl;

		Platform_LogInstrument("[FPS60] lap %d: race clock %d, real %d ms, logic frames %u (at 60: %u), speed %u%%, top kart speed %d\n", s_lap + 1,
		                       gGT->elapsedEventTime - s_lapClock, (dv * 1000) / 60, frames, at60, (dv > 0) ? (half * 100u) / (unsigned)dv : 0u,
		                       s_lapTopSpeed);
		s_lap = d->lapIndex;
		s_lapVbl = vbl;
		s_lapClock = gGT->elapsedEventTime;
		s_lapTopSpeed = 0;
		s_lapStats = g_fps60Stats;
	}
}
#endif

void Fps60_AfterTimerTick(struct GameTracker *gGT)
{
	int elapsed = gGT->elapsedTimeMS;

	if (gGT->timer < s_fps60Lag)
	{
		s_fps60Lag = 0;
	}

	if (g_fps60Full == 0)
	{
		s_fps60Lag++;
	}

	s_fps60ElapsedAcc += elapsed;
	if (g_fps60Full != 0)
	{
		// Same clamp as MainFrame_GameLogic's own; at 30 this is elapsed.
		s_fps60Elapsed30 = (s_fps60ElapsedAcc > 0x40) ? 0x40 : s_fps60ElapsedAcc;
		s_fps60ElapsedAcc = 0;
	}

#ifdef CTR_NATIVE
	g_fps60Stats.logicFrames++;
	g_fps60Stats.halfTicks += g_fps60 ? 1u : 2u;
	g_fps60Stats.frames60 += g_fps60 ? 1u : 0u;
	g_fps60Stats.ticks30 += g_fps60Full ? 1u : 0u;
#endif
#if defined(CTR_NATIVE) && defined(CTR_INTERNAL)
	Fps60_LapLog(gGT);
#endif
}

int Fps60_Timer30(void)
{
	struct GameTracker *gGT = sdata->gGT;

	// Inside the 30 Hz world gGT->timer is already the 30 Hz counter.
	if (g_fps60In30 != 0)
	{
		return gGT->timer;
	}

	return gGT->timer - s_fps60Lag;
}

void Fps60_Enter30(void)
{
	struct GameTracker *gGT = sdata->gGT;
	int depth = g_fps60In30++;

	// Also at 30 once the game has run at 60 (lag != 0): the 30 Hz world's
	// timer must stay continuous across a mode switch. Never at 60: no swap.
	if ((g_fps60 == 0) && (s_fps60Lag == 0))
	{
		s_fps60Swapped[depth < FPS60_NEST_MAX ? depth : FPS60_NEST_MAX - 1] = 0;
		return;
	}

	if (depth < FPS60_NEST_MAX)
	{
		s_fps60SavedTimer[depth] = gGT->timer;
		s_fps60SavedElapsed[depth] = gGT->elapsedTimeMS;
		s_fps60SavedFrameCounter[depth] = sdata->frameCounter;
		s_fps60Swapped[depth] = 1;
	}

	// Nested calls see the values the outer one installed.
	if (depth == 0)
	{
		gGT->timer = gGT->timer - s_fps60Lag;
		gGT->elapsedTimeMS = s_fps60Elapsed30;
		// The main loop's frame counter (MainMain) runs ahead the same way.
		sdata->frameCounter = sdata->frameCounter - s_fps60Lag;
	}
}

void Fps60_Leave30(void)
{
	struct GameTracker *gGT = sdata->gGT;
	int depth = --g_fps60In30;

	if ((depth >= 0) && (depth < FPS60_NEST_MAX) && (s_fps60Swapped[depth] != 0))
	{
		s_fps60Swapped[depth] = 0;
		gGT->timer = s_fps60SavedTimer[depth];
		gGT->elapsedTimeMS = s_fps60SavedElapsed[depth];
		sdata->frameCounter = s_fps60SavedFrameCounter[depth];
	}
}

void Fps60_BeforeSubmit(struct GameTracker *gGT)
{
	g_fps60Allowed = Fps60_ContextAllowed(gGT);
}
