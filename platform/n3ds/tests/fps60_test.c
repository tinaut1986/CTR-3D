/*
 * Host test for the 60 fps logic clock (game/MAIN/MainFps60.c) and the frame
 * pacing policy (platform/n3ds/n3ds_framepace.c). Run with
 * platform/n3ds/tests/run-fps60-tests.py.
 *
 * The main loop is simulated the way MainMain / MainFrame_GameLogic drive
 * the hooks: BeginFrame, timer++ + elapsedTimeMS, AfterTimerTick, the 30 Hz
 * world inside Enter30/Leave30 on full frames, BeforeSubmit.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "common.h"

/* ---------------------------------------------------------------- stubs --- */
static struct GameTracker s_gt;
static struct SData s_sd = {&s_gt, 0, {LOAD_IDLE}, NULL};
struct SData *sdata = &s_sd;
static int s_lastVbl = 2;
int LOAD_IsOpen_Podiums(void) { return 0; }
int Platform_LastFrameVBlanks(void) { return s_lastVbl; }

/* framepace.c's outside world */
int g_dbg_stereo = 1;
static int s_vblankCount;
static int s_oldClass;
static int s_logLines;
int Platform_GetVBlankCount(void) { return s_vblankCount; }
int N3DS_FramePaceOldClass(void) { return s_oldClass; }
unsigned N3DS_FramePaceRightEyeUs(int *stereo) { *stereo = 0; return 0; }
void N3DS_DebugLog(const char *msg)
{
	s_logLines++;
	if (getenv("FPS60_TEST_VERBOSE") != NULL)
	{
		printf("    log: %s\n", msg);
	}
}

#include "../../../game/MAIN/MainFps60.c"
#include "../n3ds_framepace.c"

static int s_fail;
#define CHECK(c, ...)                                   \
	do                                                  \
	{                                                   \
		if (!(c))                                       \
		{                                               \
			printf("FAIL %s:%d: ", __FILE__, __LINE__); \
			printf(__VA_ARGS__);                        \
			printf("\n");                               \
			s_fail++;                                   \
		}                                               \
	} while (0)

/* --------------------------------------------------------- logic clock --- */
static int s_seen30Timer = -1;   /* last gGT->timer the 30 Hz world saw */
static int s_ticks30;
static int s_halfTicks;
static int s_vblanks;
static int s_acc30;

static void reset_game(void)
{
	memset(&s_gt, 0, sizeof(s_gt));
	s_gt.levelID = 3;
	s_sd.frameCounter = 0;
	g_fps60 = 0;
	g_fps60Full = 1;
	g_fps60In30 = 0;
	g_fps60Allowed = 1;
	s_fps60Lag = 0;
	s_fps60HalfNext = 0;
	s_fps60ElapsedAcc = 0;
	s_seen30Timer = -1;
	s_ticks30 = 0;
	s_halfTicks = 0;
	s_vblanks = 0;
	s_acc30 = 0;
	s_lastVbl = 2;
}

/* One main-loop frame that the previous flip showed for `vbl` vblanks. */
static void game_frame(int vbl)
{
	s_lastVbl = vbl;
	s_sd.frameCounter++;
	Fps60_BeginFrame(&s_gt);
	s_gt.timer++;
	s_gt.elapsedTimeMS = 16 * vbl; /* ~(ms << 5) / 100 per vblank */
	Fps60_AfterTimerTick(&s_gt);
	s_halfTicks += g_fps60 ? 1 : 2;
	s_vblanks += vbl;
	s_acc30 += s_gt.elapsedTimeMS;

	if (g_fps60Full)
	{
		Fps60_Enter30();
		/* The 30 Hz world's clock advances by exactly one per tick. */
		if (s_seen30Timer >= 0)
		{
			CHECK(s_gt.timer == s_seen30Timer + 1, "30 Hz timer %d after %d", s_gt.timer, s_seen30Timer);
		}
		CHECK(Fps60_Timer30() == s_gt.timer, "Timer30 inside the 30 Hz world");
		s_seen30Timer = s_gt.timer;
		/* It sees the time since its last tick (clamped like retail). */
		CHECK(s_gt.elapsedTimeMS == ((s_acc30 > 0x40) ? 0x40 : s_acc30), "elapsed30 %d expected %d", s_gt.elapsedTimeMS, s_acc30);
		s_acc30 = 0;
		Fps60_Enter30(); /* nested: no second swap */
		CHECK(s_gt.timer == s_seen30Timer, "nested Enter30 swapped again");
		Fps60_Leave30();
		Fps60_Leave30();
		s_ticks30++;
	}
	CHECK(g_fps60In30 == 0, "unbalanced");
	Fps60_BeforeSubmit(&s_gt);
}

static void test_never60(void)
{
	reset_game();
	for (int i = 0; i < 1000; i++)
	{
		const int t0 = s_gt.timer;

		game_frame((i % 7 == 3) ? 3 : 2);
		CHECK(g_fps60 == 0 && g_fps60Full == 1, "frame %d not retail", i);
		CHECK(Fps60_Timer30() == s_gt.timer, "Timer30 != timer at 30");
		CHECK(s_gt.timer == t0 + 1, "timer");
	}
	CHECK(s_ticks30 == 1000, "30 Hz world skipped at 30 fps");
	printf("ok   never at 60: retail clock\n");
}

static void test_steady60(void)
{
	reset_game();
	for (int i = 0; i < 600; i++)
	{
		game_frame(1);
	}
	CHECK(s_ticks30 == 300, "30 Hz ticks %d for 600 vblanks", s_ticks30);
	CHECK(s_halfTicks == s_vblanks, "half ticks %d vblanks %d", s_halfTicks, s_vblanks);
	CHECK(Fps60_Timer30() <= 302, "Timer30 %d", Fps60_Timer30());
	printf("ok   steady 60: 30 Hz world at 30, frame-counted speed 100%%\n");
}

static void test_mixed(void)
{
	/* Frames that miss now and then (OFF, or AUTO at its edge). */
	static const int pattern[] = {1, 1, 2, 1, 1, 1, 3, 1, 2, 2, 1};
	int n = 0;

	reset_game();
	for (int i = 0; i < 2000; i++)
	{
		game_frame(pattern[i % 11]);
		n++;
	}
	/* Each frame is paid from the previous interval: the sum matches up to
	 * the first/last frame (and 3-vblank frames tick 1, like retail at 20). */
	{
		const int threes = 2000 / 11 + 1;
		const int expect = s_vblanks - threes; /* a 3-vblank frame pays 2 half ticks */

		CHECK(abs(s_halfTicks - expect) <= 3, "half ticks %d expected ~%d (vblanks %d)", s_halfTicks, expect, s_vblanks);
	}
	printf("ok   mixed 1/2/3-vblank frames: speed kept, 30 Hz clock continuous (%d frames)\n", n);
}

static void test_switch_back(void)
{
	reset_game();
	for (int i = 0; i < 100; i++)
	{
		game_frame(1);
	}
	for (int i = 0; i < 100; i++)
	{
		game_frame(2); /* continuity is CHECKed inside game_frame */
	}
	for (int i = 0; i < 101; i++)
	{
		game_frame(1);
	}
	CHECK(s_fps60Lag > 0, "lag");
	g_fps60Allowed = 0; /* a menu: 30 even after 1-vblank flips */
	game_frame(1);
	CHECK(g_fps60 == 0, "60 outside an allowed context");
	s_gt.gameMode1 = PAUSE_ALL;
	g_fps60Allowed = 1;
	game_frame(1);
	CHECK(g_fps60 == 0, "60 while paused");
	printf("ok   60 -> 30 -> 60: 30 Hz world clock continuous; menus/pause stay 30\n");
}

/* ---------------------------------------------------------- frame pace --- */
static void reset_pace(int mode)
{
	g_dbg_frameCap = mode;
	s_paceVbl = 2;
	s_paceDwell = 0;
	s_paceUpStreak = 0;
	s_paceUpNeed60 = N3DS_PACE_UP_WINDOWS;
	s_pace60Windows = 0;
	s_paceWinN = 0;
	s_paceTarget = 2;
	s_logLines = 0;
	g_fps60Allowed = 1;
}

/* work(us) for a frame submitted at interval vbl. Returns the changes. */
static int run_pace(int frames, unsigned (*work)(int vbl, int i), int *at60)
{
	int changes = 0;
	int prev = s_paceTarget;

	*at60 = 0;
	for (int i = 0; i < frames; i++)
	{
		N3DS_FramePaceNoteFrame(work(s_paceTarget, i));
		const int v = N3DS_FramePaceNextFlip();

		s_vblankCount += v;
		*at60 += (v == 1);
		changes += (v != prev);
		prev = v;
	}
	return changes;
}

static unsigned work_light(int vbl, int i) { (void)vbl; (void)i; return 10000; }
static unsigned work_heavy60(int vbl, int i) { (void)vbl; (void)i; return 20000; }
static unsigned work_o3ds(int vbl, int i) { (void)vbl; (void)i; return 40000; }
/* Fits at 30 with margin, misses at 60 (e.g. lost CPU/GPU overlap). */
static unsigned work_edge(int vbl, int i) { (void)i; return (vbl == 1) ? 18000 : 12000; }

static void test_pace(void)
{
	int at60;
	int ch;

	reset_pace(0);
	ch = run_pace(600, work_light, &at60);
	CHECK(s_paceVbl == 1 && at60 > 450, "AUTO light load: vbl %d at60 %d", s_paceVbl, at60);
	CHECK(ch == 1, "AUTO light load changes %d", ch);
	printf("ok   AUTO, 10 ms frames: up to 60 in %d frames, stays\n", 600 - at60);

	g_fps60Allowed = 0;
	CHECK(N3DS_FramePaceFlipVBlanks() == 2, "menu at 60");
	g_fps60Allowed = 1;

	reset_pace(0);
	s_paceVbl = 1;
	ch = run_pace(600, work_heavy60, &at60);
	CHECK(s_paceVbl == 2, "AUTO 20 ms frames: vbl %d", s_paceVbl);
	printf("ok   AUTO, 20 ms frames: 60 -> 30 after %d frames\n", at60);

	reset_pace(0);
	ch = run_pace(600, work_o3ds, &at60);
	CHECK(s_paceVbl == 3 && at60 == 0, "AUTO 40 ms frames: vbl %d", s_paceVbl);
	printf("ok   AUTO, 40 ms frames (Old 3DS): 30 -> 20, never 60\n");

	reset_pace(0);
	ch = run_pace(3600, work_edge, &at60); /* 3600 frames at the edge (~2 min at 30) */
	CHECK(ch <= 12, "AUTO at the edge oscillates: %d changes in 3600 frames", ch);
	CHECK(at60 <= 6 * 40, "AUTO at the edge: %d frames at 60 (each try should fail in one window)", at60);
	{
		int at60b;
		const int ch2 = run_pace(3600, work_edge, &at60b); /* the next ~2 minutes: one try a minute */

		CHECK(ch2 <= 4, "AUTO at the edge, next 3600 frames: %d changes", ch2);
		printf("ok   AUTO at the 60 edge: %d rate changes in the first 3600 frames, %d in the next (backoff %d windows)\n", ch, ch2, s_paceUpNeed60);
	}

	reset_pace(1);
	ch = run_pace(600, work_light, &at60);
	CHECK(at60 == 0 && ch == 0, "30 FPS went to 60");
	printf("ok   30: always 2 vblanks\n");

	reset_pace(2);
	ch = run_pace(100, work_heavy60, &at60);
	CHECK(at60 == 100, "OFF: %d", at60);
	g_fps60Allowed = 0;
	CHECK(N3DS_FramePaceFlipVBlanks() == 2, "OFF in a menu");
	printf("ok   OFF: 1 vblank wherever allowed\n");
}

int main(void)
{
	test_never60();
	test_steady60();
	test_mixed();
	test_switch_back();
	test_pace();
	if (s_fail != 0)
	{
		printf("%d check(s) failed\n", s_fail);
		return 1;
	}
	printf("all fps60 tests passed\n");
	return 0;
}
