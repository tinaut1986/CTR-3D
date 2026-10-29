/*
 * Frame pacing policy (FRAME SKIP AUTO / 30 / OFF, Old-3DS AUTO stereo).
 * #included by n3ds_backend.c (one TU with it, like before the split) and
 * compiled alone by the host test platform/n3ds/tests/framepace_test.c, so it
 * reaches nothing of libctru: the console model, the right eye's cost and the
 * log come through the functions declared below.
 */
#include <stdio.h>

#include "n3ds_backend.h"
#include "ctr_fps60.h" /* g_fps60Allowed, g_fps60Stats: plain ints, no enums */

int Platform_GetVBlankCount(void);
extern int g_dbg_stereo;
/* n3ds_backend.c: the right eye's last measured cost, *stereo = whether the
 * frame rendered it. */
unsigned N3DS_FramePaceRightEyeUs(int *stereo);

/* ----------------------------------------------------------- frame pace --- */
/*
 * See n3ds_backend.h's frame-pace block. Everything here runs on the game
 * thread, once per flip, from Platform_FlipVBlanks (RenderSubmit): no locks.
 *
 * FRAME SKIP (g_dbg_frameCap, cfg `fpsmode`): 0 AUTO, 1 30 (retail), 2 OFF.
 * The flip interval is 1 (60 fps), 2 (30) or 3 (20) emulated vblanks; a game
 * context that must stay at 30 (menus, pause, cutscenes, loads:
 * g_fps60Allowed, game/MAIN/MainFps60.c) never gets 1.
 *
 * AUTO, every console, decided per window of N3DS_PACE_WINDOW flips:
 *   down one step (60 -> 30 -> 20) when N3DS_PACE_MISS_DOWN or more frames
 *            of the window missed the current interval's budget;
 *   up one step when N3DS_PACE_UP_WINDOWS windows in a row (for 30 -> 60:
 *            s_paceUpNeed60, see below) had at most one frame over the
 *            faster interval's budget minus a margin (4 ms for 20 -> 30,
 *            N3DS_PACE_UP60_MARGIN_US for 30 -> 60).
 * After any change the next N3DS_PACE_DWELL windows decide nothing (except
 * dropping a 60 that misses, which is never held), so the two thresholds
 * cannot chase each other. 60 needs a window of gameplay frames
 * (at least half of it with g_fps60Allowed) to rise or to be judged, so menus
 * and loads neither promote it nor count against it. Backoff: a 60 that falls
 * back within N3DS_PACE_SHORT60 windows doubles the quiet windows the next
 * 30 -> 60 needs (2, 4, ... N3DS_PACE_UP60_MAX); one that holds
 * N3DS_PACE_LONG60 windows resets it. A scene that sits on the edge of 60 thus
 * settles at 30 instead of flickering between the two every second.
 * Frames over N3DS_PACE_IGNORE_US are loads or stalls, not a frame rate, and
 * are not counted. OFF: 1 whenever the context allows it (each frame then
 * takes the vblanks it needs); 30: always 2.
 *
 * AUTO stereo (g_dbg_stereo == 2, Old-3DS class only), after the cap decision,
 * against the slowest cap the option allows (3 vblanks unless the player forced
 * 30: 3D at 20 beats 2D at 30, the slider is up): the right eye goes off when
 * the window missed that budget N3DS_PACE_MISS_DOWN times or more but its
 * average frame minus
 * the average right-eye cost would fit with N3DS_PACE_ST_MARGIN_US to spare;
 * it comes back when, N3DS_PACE_UP_WINDOWS windows in a row, the average
 * frame plus the last measured right-eye cost fits with the same margin. The
 * slider still gates frame by frame; a window without stereo frames (slider
 * down) decides nothing about stereo.
 */
#define N3DS_PACE_WINDOW         32
#define N3DS_PACE_MISS_DOWN      8
#define N3DS_PACE_UP_WINDOWS     2
#define N3DS_PACE_DWELL          2
#define N3DS_PACE_VBLANK_US      16667u
#define N3DS_PACE_UP_US          (2u * N3DS_PACE_VBLANK_US - 4000u)
#define N3DS_PACE_UP60_MARGIN_US 3000u
#define N3DS_PACE_UP60_US        (N3DS_PACE_VBLANK_US - N3DS_PACE_UP60_MARGIN_US)
#define N3DS_PACE_UP60_MAX       64
#define N3DS_PACE_SHORT60        8
#define N3DS_PACE_LONG60         32
#define N3DS_PACE_IGNORE_US      250000u
#define N3DS_PACE_ST_MARGIN_US   3000u

int g_dbg_frameCap = 0;

static int s_paceVbl = 2;         /* the AUTO cap's current flip interval: 1, 2 or 3 */
static int s_paceStereoOff = 0;   /* AUTO stereo dropped the right eye */
static unsigned s_paceWinN;
static unsigned s_paceWinOver1; /* frames targeted at 1 vblank that took longer than 16.7 ms */
static unsigned s_paceWinAt1;   /* frames targeted at 1 vblank */
static unsigned s_paceWinOver2; /* frames over 2 vblanks (33.3 ms) */
static unsigned s_paceWinOver3; /* frames over 3 vblanks (50 ms) */
static unsigned s_paceWinOverUp;
static unsigned s_paceWinOverUp60;
static unsigned s_paceWinAllowed; /* frames whose context allowed 60 */
static unsigned long long s_paceWinWorkUs;
static unsigned s_paceWinStereoN;
static unsigned long long s_paceWinRightUs;
static unsigned s_paceRightLastUs; /* average right-eye cost of the last stereo window */
static int s_paceUpStreak;
static int s_paceStereoUpStreak;
static int s_paceDwell;
static int s_paceUpNeed60 = N3DS_PACE_UP_WINDOWS; /* backoff for 30 -> 60 */
static int s_pace60Windows;                      /* windows since AUTO went to 60 */
/* [PACE] report window (the [PERF] cadence), separate from the decision window. */
static unsigned s_paceRepN;
static unsigned s_paceRepMiss;
static unsigned s_paceRepMaxUs;
static unsigned long long s_paceRepWorkUs;
static unsigned s_paceRepChanges;
static unsigned s_paceRepVbl0;     /* vblank counter at the start of the window */
static struct Fps60Stats s_paceRepFps0; /* game counters at the start of the window */


/* The interval the AUTO policy would pick in this context. */
static int N3DS_FramePaceAutoVbl(void)
{
	return (s_paceVbl == 1 && !g_fps60Allowed) ? 2 : s_paceVbl;
}

int N3DS_FramePaceFlipVBlanks(void)
{
	if (g_dbg_frameCap == 1)
	{
		return 2;
	}
	if (g_dbg_frameCap == 2)
	{
		return g_fps60Allowed ? 1 : 2;
	}
	return N3DS_FramePaceAutoVbl();
}

int N3DS_FramePaceStereoSuppressed(void)
{
	return (g_dbg_stereo == 2) && s_paceStereoOff && N3DS_FramePaceOldClass();
}

static void N3DS_FramePaceChange(int vbl, const char *why)
{
	char msg[200];
	const unsigned avgWork = (s_paceWinN != 0) ? (unsigned)(s_paceWinWorkUs / s_paceWinN) : 0;

	if ((s_paceVbl == 1) && (vbl == 2))
	{
		/* Backoff: a 60 that did not last makes the next try wait longer. */
		if (s_pace60Windows < N3DS_PACE_SHORT60)
		{
			s_paceUpNeed60 = (s_paceUpNeed60 * 2 > N3DS_PACE_UP60_MAX) ? N3DS_PACE_UP60_MAX : (s_paceUpNeed60 * 2);
		}
	}
	snprintf(msg, sizeof(msg), "[PACE] auto cap %d -> %d fps (%s, avg work %u us, next 30->60 after %d quiet windows)", 60 / s_paceVbl, 60 / vbl, why, avgWork,
	         s_paceUpNeed60);
	s_paceVbl = vbl;
	s_pace60Windows = 0;
	s_paceUpStreak = 0;
	s_paceDwell = N3DS_PACE_DWELL;
	s_paceRepChanges++;
	N3DS_DebugLog(msg);
}

static void N3DS_FramePaceDecide(void)
{
	const unsigned avgWork = (s_paceWinN != 0) ? (unsigned)(s_paceWinWorkUs / s_paceWinN) : 0;
	/* Enough gameplay frames to judge 60 (menus and loads stay at 30). */
	const int gameplay = (s_paceWinAllowed * 2 >= s_paceWinN);
	char msg[160];

	if (s_paceVbl == 1)
	{
		s_pace60Windows++;
		if (s_pace60Windows >= N3DS_PACE_LONG60)
		{
			s_paceUpNeed60 = N3DS_PACE_UP_WINDOWS;
		}
	}
	/* A 60 that does not hold drops at once, dwell or not: each window of it
	 * is a visible stutter, and the backoff (not the dwell) spaces the tries. */
	if ((g_dbg_frameCap == 0) && (s_paceVbl == 1) && (s_paceWinAt1 * 2 >= s_paceWinN) && (s_paceWinOver1 >= N3DS_PACE_MISS_DOWN))
	{
		snprintf(msg, sizeof(msg), "%u/%u frames over 1 vblank", s_paceWinOver1, s_paceWinAt1);
		N3DS_FramePaceChange(2, msg);
		return;
	}
	if (s_paceDwell > 0)
	{
		s_paceDwell--;
		return;
	}

	if (g_dbg_frameCap == 0)
	{
		if ((s_paceVbl == 2) && (s_paceWinOver2 >= N3DS_PACE_MISS_DOWN))
		{
			snprintf(msg, sizeof(msg), "%u/%u frames over 2 vblanks", s_paceWinOver2, s_paceWinN);
			N3DS_FramePaceChange(3, msg);
			return;
		}
		if (s_paceVbl == 3)
		{
			s_paceUpStreak = (s_paceWinOverUp <= 1) ? (s_paceUpStreak + 1) : 0;
			if (s_paceUpStreak >= N3DS_PACE_UP_WINDOWS)
			{
				snprintf(msg, sizeof(msg), "%u frames over %u us", s_paceWinOverUp, N3DS_PACE_UP_US);
				N3DS_FramePaceChange(2, msg);
				return;
			}
		}
		else if ((s_paceVbl == 2) && gameplay)
		{
			s_paceUpStreak = (s_paceWinOverUp60 <= 1) ? (s_paceUpStreak + 1) : 0;
			if (s_paceUpStreak >= s_paceUpNeed60)
			{
				snprintf(msg, sizeof(msg), "%u frames over %u us", s_paceWinOverUp60, N3DS_PACE_UP60_US);
				N3DS_FramePaceChange(1, msg);
				return;
			}
		}
	}
	if ((g_dbg_stereo != 2) || !N3DS_FramePaceOldClass())
	{
		s_paceStereoOff = 0;
		return;
	}
	{
		/* The budget 3D is held to: the slowest cap AUTO would pick (3D at 20
		 * beats 2D at 30 -- the player raised the slider), or the forced one. */
		const int stVbl = (g_dbg_frameCap == 1) ? 2 : 3;
		const unsigned budget = (unsigned)stVbl * N3DS_PACE_VBLANK_US;
		const unsigned over = (stVbl == 2) ? s_paceWinOver2 : s_paceWinOver3;

		if (!s_paceStereoOff && (s_paceWinStereoN * 2 >= s_paceWinN) && (s_paceWinStereoN != 0))
		{
			const unsigned avgRight = (unsigned)(s_paceWinRightUs / s_paceWinStereoN);

			s_paceRightLastUs = avgRight;
			if ((over >= N3DS_PACE_MISS_DOWN) && ((avgWork - ((avgRight < avgWork) ? avgRight : avgWork)) + N3DS_PACE_ST_MARGIN_US <= budget))
			{
				s_paceStereoOff = 1;
				s_paceStereoUpStreak = 0;
				s_paceDwell = N3DS_PACE_DWELL;
				s_paceRepChanges++;
				snprintf(msg, sizeof(msg), "[PACE] auto 3D: right eye OFF (%u/%u frames over %u us, avg work %u us, right eye %u us)", over, s_paceWinN,
				         budget, avgWork, avgRight);
				N3DS_DebugLog(msg);
			}
		}
		else if (s_paceStereoOff)
		{
			s_paceStereoUpStreak = ((avgWork + s_paceRightLastUs + N3DS_PACE_ST_MARGIN_US) <= budget) ? (s_paceStereoUpStreak + 1) : 0;
			if (s_paceStereoUpStreak >= N3DS_PACE_UP_WINDOWS)
			{
				s_paceStereoOff = 0;
				s_paceStereoUpStreak = 0;
				s_paceDwell = N3DS_PACE_DWELL;
				s_paceRepChanges++;
				snprintf(msg, sizeof(msg), "[PACE] auto 3D: right eye back ON (avg work %u us + right eye %u us fits %u us)", avgWork, s_paceRightLastUs,
				         budget);
				N3DS_DebugLog(msg);
			}
		}
	}
}

/* The interval the frame being measured was submitted with. */
static int s_paceTarget = 2;

int N3DS_FramePaceNextFlip(void)
{
	s_paceTarget = N3DS_FramePaceFlipVBlanks();
	return s_paceTarget;
}

void N3DS_FramePaceNoteFrame(unsigned workUs)
{
	const unsigned vbl = (unsigned)s_paceTarget;
	int stereoFrame = 0;
	const unsigned rightUs = N3DS_FramePaceRightEyeUs(&stereoFrame);

	if (workUs > N3DS_PACE_IGNORE_US)
	{
		return;
	}

	s_paceWinN++;
	s_paceWinWorkUs += workUs;
	s_paceWinAllowed += g_fps60Allowed ? 1u : 0u;
	if (vbl == 1)
	{
		s_paceWinAt1++;
		s_paceWinOver1 += (workUs > N3DS_PACE_VBLANK_US) ? 1u : 0u;
	}
	if (workUs > N3DS_PACE_UP60_US)
	{
		s_paceWinOverUp60++;
	}
	if (workUs > (2u * N3DS_PACE_VBLANK_US))
	{
		s_paceWinOver2++;
	}
	if (workUs > (3u * N3DS_PACE_VBLANK_US))
	{
		s_paceWinOver3++;
	}
	if (workUs > N3DS_PACE_UP_US)
	{
		s_paceWinOverUp++;
	}
	if (stereoFrame)
	{
		s_paceWinStereoN++;
		s_paceWinRightUs += rightUs;
	}

	s_paceRepN++;
	s_paceRepWorkUs += workUs;
	s_paceRepMiss += (workUs > (vbl * N3DS_PACE_VBLANK_US)) ? 1u : 0u;
	if (workUs > s_paceRepMaxUs)
	{
		s_paceRepMaxUs = workUs;
	}

	if (s_paceWinN >= N3DS_PACE_WINDOW)
	{
		N3DS_FramePaceDecide();
		s_paceWinN = 0;
		s_paceWinAt1 = 0;
		s_paceWinOver1 = 0;
		s_paceWinOverUp60 = 0;
		s_paceWinAllowed = 0;
		s_paceWinOver2 = 0;
		s_paceWinOver3 = 0;
		s_paceWinOverUp = 0;
		s_paceWinWorkUs = 0;
		s_paceWinStereoN = 0;
		s_paceWinRightUs = 0;
	}
}

void N3DS_FramePaceFormat(char *out, int outSize)
{
	const unsigned n = (s_paceRepN != 0) ? s_paceRepN : 1;
	const unsigned vblNow = (unsigned)Platform_GetVBlankCount();
	const unsigned vbl = vblNow - s_paceRepVbl0;
	const struct Fps60Stats f = g_fps60Stats;
	const unsigned logic = f.logicFrames - s_paceRepFps0.logicFrames;
	const unsigned at60 = f.frames60 - s_paceRepFps0.frames60;
	const unsigned half = f.halfTicks - s_paceRepFps0.halfTicks;
	const unsigned t30 = f.ticks30 - s_paceRepFps0.ticks30;

	/* cap = the flip interval in force (fps), mode = the option, miss = frames
	 * of the window over the cap's budget, 3d = the stereo option and whether
	 * AUTO has the right eye off, rEye = last stereo window's right-eye cost.
	 * Game speed (game/MAIN/MainFps60.c): logic = game frames that ticked,
	 * at60 = of which at 60 Hz, speed = frame-counted logic speed in % of
	 * retail-at-30 (half ticks per vblank; 100 at a steady 60 or 30, 67 at
	 * 20, like retail), t30 = 30 Hz-world ticks (~vbl/2 at full speed), vbl =
	 * emulated vblanks of the window, sw = 60 <-> 30 logic switches. Paused
	 * or loading windows read low: nothing ticks then. */
	snprintf(out, (size_t)outSize,
	         "[PACE] cap=%d mode=%s old=%d frames=%u avgWorkUs=%u maxWorkUs=%u miss=%u changes=%u 3d=%s%s rEyeUs=%u logic=%u at60=%u speed=%u%% t30=%u vbl=%u "
	         "sw=%u up60=%d",
	         60 / N3DS_FramePaceFlipVBlanks(), (g_dbg_frameCap == 1) ? "30" : (g_dbg_frameCap == 2) ? "off" : "auto", N3DS_FramePaceOldClass(), s_paceRepN,
	         (unsigned)(s_paceRepWorkUs / n), s_paceRepMaxUs, s_paceRepMiss, s_paceRepChanges, (g_dbg_stereo == 0) ? "off" : (g_dbg_stereo == 2) ? "auto" : "on",
	         N3DS_FramePaceStereoSuppressed() ? "(2D)" : "", s_paceRightLastUs, logic, at60, (vbl != 0) ? (unsigned)((half * 100u) / vbl) : 0u, t30, vbl,
	         f.switches - s_paceRepFps0.switches, s_paceUpNeed60);
	s_paceRepN = 0;
	s_paceRepMiss = 0;
	s_paceRepMaxUs = 0;
	s_paceRepWorkUs = 0;
	s_paceRepChanges = 0;
	s_paceRepVbl0 = vblNow;
	s_paceRepFps0 = f;
}
