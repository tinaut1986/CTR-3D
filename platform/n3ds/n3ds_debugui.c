/*
 * Bottom-screen debug/info UI (see n3ds_debugui.h for the user-facing summary).
 *
 * RENDERING APPROACH -- why a CPU raster straight into the bottom framebuffer:
 *   - The bottom screen already has two owners: the minimap, drawn through a
 *     citro3d render target inside the game's frame (N3DS_BottomMapDraw_*), and
 *     libctru's software console. Adding a citro2d pass would put a third
 *     GPU consumer inside the C3D_FrameBegin/End structure that the render
 *     worker (core 2) drives, on a frame that is already GPU-bound.
 *   - This UI instead follows the console's path: it writes pixels into the
 *     bottom gfx framebuffer on the game thread, and N3DS_GfxPresent flushes
 *     and swaps it as it always did. No GPU work, no render-thread
 *     involvement, no shader/state changes.
 *   - Text uses the 5x7 bitmap font of the author's mzm port (the same
 *     "rasterise from scratch, redraw only when something changed or ~10 Hz"
 *     convention as its port_bottom_ui_3ds.c), scaled 2x for readability.
 *     libctru's console font is not exposed, and the game's own PS1 font goes
 *     through the GPU packet path, which this must not touch.
 *   - Cost: closed = one mask test per input poll (N3DS_ReadButtons). Open =
 *     a ~230 KB framebuffer repaint at most every 100 ms plus a ring-buffer
 *     write per frame; the framebuffer is single-buffered while open so the
 *     partial redraws never flicker between the two buffers.
 *
 * THREADING: everything here runs on the game thread (core 0): input poll in
 * N3DS_ReadButtons, Tick from NativeRenderer_SwapWindow. It creates no thread
 * and touches no core affinity. The FTP button only starts/stops the existing
 * FTP thread (cores 3/2, see n3ds_ftp.c) without blocking.
 */

#include <3ds.h>
#include <arpa/inet.h>
#include <malloc.h>
#include <netinet/in.h>
#include <stdio.h>
#include <string.h>

#include "n3ds_backend.h"
#include "n3ds_debugui.h"
#include "n3ds_ftp.h"

#define UI_W 320
#define UI_H 240
#define UI_RING 120 /* frame-time history: also the INFO "worst of the last N frames" window */
#define UI_REDRAW_US 100000ULL

volatile int g_n3dsDebugUiOpen = 0;

/* Runtime switches shown/toggled here; each is defined by its owning TU. */
extern int g_dbg_sdLog;
extern int g_dbg_stereo;
extern int g_dbg_drawMerge;
extern int g_dbg_renderThread;
extern int g_dbg_btmMap;

/* ------------------------------------------------------------------ state --- */

enum
{
	TAB_INFO = 0,
	TAB_PERF,
	TAB_DEBUG,
	TAB_COUNT
};

static int s_tab = TAB_INFO;
static int s_dirty = 0;           /* frames still to repaint unconditionally */
static u64 s_lastDrawTick = 0;
static u64 s_lastFrameTick = 0;
static u32 s_ring[UI_RING];       /* frame times, microseconds */
static int s_ringHead = 0;
static int s_ringCount = 0;
static u32 s_lastDtUs = 0;
static struct N3DS_DebugUIGameInfo s_gi;
static unsigned int s_prevDraws = 0, s_prevVerts = 0;
static unsigned int s_drawsPerFrame = 0, s_vertsPerFrame = 0;
static char s_msg[40];
static int s_pressBtn = -1;
static int s_pressFrames = 0;
static int s_isNew3DS = -1;

/* ----------------------------------------------------------- pixel raster --- */

static u8 *s_fb;

#define RGB(r, g, b) (((u32)(r) << 16) | ((u32)(g) << 8) | (u32)(b))
#define COL_BG RGB(0x10, 0x12, 0x1A)
#define COL_TEXT RGB(0xFF, 0xFF, 0xFF)
#define COL_LABEL RGB(0x9A, 0xA6, 0xC0)
#define COL_GOOD RGB(0x50, 0xE0, 0x80)
#define COL_WARN RGB(0xFF, 0xC0, 0x40)
#define COL_BAD RGB(0xFF, 0x60, 0x60)
#define COL_TAB_ON RGB(0x2A, 0x6F, 0xDB)
#define COL_TAB_OFF RGB(0x2C, 0x30, 0x3C)
#define COL_BTN_ON RGB(0x1C, 0x7A, 0x3A)
#define COL_BTN_OFF RGB(0x5A, 0x24, 0x28)
#define COL_BTN_ACT RGB(0x2C, 0x44, 0x80)
#define COL_PRESS RGB(0xFF, 0xFF, 0x60)

/* Framebuffer is BGR8, column-major, origin bottom-left of the rotated panel:
 * pixel (x, y from top) lives at ((x * 240) + (239 - y)) * 3. */
static void ui_rect(int x, int y, int w, int h, u32 rgb)
{
	const u8 b = (u8)(rgb & 0xFF), g = (u8)((rgb >> 8) & 0xFF), r = (u8)((rgb >> 16) & 0xFF);
	int cx, k;

	if (x < 0) { w += x; x = 0; }
	if (y < 0) { h += y; y = 0; }
	if (x + w > UI_W) { w = UI_W - x; }
	if (y + h > UI_H) { h = UI_H - y; }
	if ((w <= 0) || (h <= 0))
	{
		return;
	}
	for (cx = x; cx < x + w; cx++)
	{
		u8 *p = s_fb + (((cx * UI_H) + (UI_H - (y + h))) * 3);

		for (k = 0; k < h; k++)
		{
			*p++ = b;
			*p++ = g;
			*p++ = r;
		}
	}
}

#ifdef CTR_INTERNAL
static void ui_frame(int x, int y, int w, int h, int t, u32 rgb)
{
	ui_rect(x, y, w, t, rgb);
	ui_rect(x, y + h - t, w, t, rgb);
	ui_rect(x, y, t, h, rgb);
	ui_rect(x + w - t, y, t, h, rgb);
}
#endif

/* 5x7 glyph rows (bit 4 = leftmost column). Digits/letters/punctuation are the
 * table from the author's mzm port (port_bottom_ui_3ds.c); '_' ',' '~' added. */
static const u8 *ui_glyph(char c)
{
	static const u8 digits[10][7] = {
		{ 0x0E, 0x11, 0x13, 0x15, 0x19, 0x11, 0x0E }, { 0x04, 0x0C, 0x04, 0x04, 0x04, 0x04, 0x0E },
		{ 0x0E, 0x11, 0x01, 0x02, 0x04, 0x08, 0x1F }, { 0x1E, 0x01, 0x01, 0x0E, 0x01, 0x01, 0x1E },
		{ 0x02, 0x06, 0x0A, 0x12, 0x1F, 0x02, 0x02 }, { 0x1F, 0x10, 0x10, 0x1E, 0x01, 0x01, 0x1E },
		{ 0x0E, 0x10, 0x10, 0x1E, 0x11, 0x11, 0x0E }, { 0x1F, 0x01, 0x02, 0x04, 0x08, 0x08, 0x08 },
		{ 0x0E, 0x11, 0x11, 0x0E, 0x11, 0x11, 0x0E }, { 0x0E, 0x11, 0x11, 0x0F, 0x01, 0x01, 0x0E },
	};
	static const u8 letters[26][7] = {
		{ 0x0E, 0x11, 0x11, 0x1F, 0x11, 0x11, 0x11 }, { 0x1E, 0x11, 0x11, 0x1E, 0x11, 0x11, 0x1E },
		{ 0x0E, 0x11, 0x10, 0x10, 0x10, 0x11, 0x0E }, { 0x1C, 0x12, 0x11, 0x11, 0x11, 0x12, 0x1C },
		{ 0x1F, 0x10, 0x10, 0x1E, 0x10, 0x10, 0x1F }, { 0x1F, 0x10, 0x10, 0x1E, 0x10, 0x10, 0x10 },
		{ 0x0E, 0x11, 0x10, 0x13, 0x11, 0x11, 0x0F }, { 0x11, 0x11, 0x11, 0x1F, 0x11, 0x11, 0x11 },
		{ 0x0E, 0x04, 0x04, 0x04, 0x04, 0x04, 0x0E }, { 0x07, 0x02, 0x02, 0x02, 0x02, 0x12, 0x0C },
		{ 0x11, 0x12, 0x14, 0x18, 0x14, 0x12, 0x11 }, { 0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x1F },
		{ 0x11, 0x1B, 0x15, 0x15, 0x11, 0x11, 0x11 }, { 0x11, 0x19, 0x15, 0x13, 0x11, 0x11, 0x11 },
		{ 0x0E, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0E }, { 0x1E, 0x11, 0x11, 0x1E, 0x10, 0x10, 0x10 },
		{ 0x0E, 0x11, 0x11, 0x11, 0x15, 0x12, 0x0D }, { 0x1E, 0x11, 0x11, 0x1E, 0x14, 0x12, 0x11 },
		{ 0x0E, 0x11, 0x10, 0x0E, 0x01, 0x11, 0x0E }, { 0x1F, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04 },
		{ 0x11, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0E }, { 0x11, 0x11, 0x11, 0x11, 0x11, 0x0A, 0x04 },
		{ 0x11, 0x11, 0x11, 0x15, 0x15, 0x15, 0x0A }, { 0x11, 0x11, 0x0A, 0x04, 0x0A, 0x11, 0x11 },
		{ 0x11, 0x11, 0x0A, 0x04, 0x04, 0x04, 0x04 }, { 0x1F, 0x01, 0x02, 0x04, 0x08, 0x10, 0x1F },
	};
	static const u8 colon[7] = { 0x00, 0x0C, 0x0C, 0x00, 0x0C, 0x0C, 0x00 };
	static const u8 dot[7] = { 0x00, 0x00, 0x00, 0x00, 0x00, 0x0C, 0x0C };
	static const u8 comma[7] = { 0x00, 0x00, 0x00, 0x00, 0x0C, 0x04, 0x08 };
	static const u8 dash[7] = { 0x00, 0x00, 0x00, 0x1F, 0x00, 0x00, 0x00 };
	static const u8 under[7] = { 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x1F };
	static const u8 slash[7] = { 0x01, 0x02, 0x02, 0x04, 0x08, 0x08, 0x10 };
	static const u8 percent[7] = { 0x13, 0x13, 0x02, 0x04, 0x08, 0x19, 0x19 };
	static const u8 lbr[7] = { 0x0E, 0x08, 0x08, 0x08, 0x08, 0x08, 0x0E };
	static const u8 rbr[7] = { 0x0E, 0x02, 0x02, 0x02, 0x02, 0x02, 0x0E };
	static const u8 plus[7] = { 0x00, 0x04, 0x04, 0x1F, 0x04, 0x04, 0x00 };
	static const u8 equal[7] = { 0x00, 0x1F, 0x00, 0x1F, 0x00, 0x00, 0x00 };
	static const u8 excl[7] = { 0x04, 0x04, 0x04, 0x04, 0x04, 0x00, 0x04 };
	static const u8 quest[7] = { 0x0E, 0x11, 0x01, 0x02, 0x04, 0x00, 0x04 };
	static const u8 gt[7] = { 0x10, 0x08, 0x04, 0x02, 0x04, 0x08, 0x10 };
	static const u8 lt[7] = { 0x01, 0x02, 0x04, 0x08, 0x04, 0x02, 0x01 };
	static const u8 tilde[7] = { 0x00, 0x00, 0x08, 0x15, 0x02, 0x00, 0x00 };
	static const u8 blank[7] = { 0, 0, 0, 0, 0, 0, 0 };

	if ((c >= '0') && (c <= '9')) { return digits[c - '0']; }
	if ((c >= 'A') && (c <= 'Z')) { return letters[c - 'A']; }
	if ((c >= 'a') && (c <= 'z')) { return letters[c - 'a']; }
	switch (c)
	{
	case ':': return colon;
	case '.': return dot;
	case ',': return comma;
	case '-': return dash;
	case '_': return under;
	case '/': return slash;
	case '%': return percent;
	case '[': case '(': return lbr;
	case ']': case ')': return rbr;
	case '+': return plus;
	case '=': return equal;
	case '!': return excl;
	case '?': return quest;
	case '>': return gt;
	case '<': return lt;
	case '~': return tilde;
	default: return blank;
	}
}

/* Returns the x after the text. scale 1 = 5x7 (6 px advance), 2 = 10x14 (12 px). */
static int ui_text(int x, int y, int scale, u32 rgb, const char *s)
{
	for (; *s != '\0'; s++)
	{
		const u8 *g = ui_glyph(*s);
		int row, col;

		for (row = 0; row < 7; row++)
		{
			for (col = 0; col < 5; col++)
			{
				if (g[row] & (0x10 >> col))
				{
					ui_rect(x + (col * scale), y + (row * scale), scale, scale, rgb);
				}
			}
		}
		x += 6 * scale;
	}
	return x;
}

static int ui_text_w(int scale, const char *s)
{
	return (int)strlen(s) * 6 * scale;
}

/* ------------------------------------------------------------------ layout --- */

#define TAB_BAR_H 28
#define CLOSE_W 32

static int ui_tab_list(int *ids)
{
	int n = 0;

	ids[n++] = TAB_INFO;
	ids[n++] = TAB_PERF;
#ifdef CTR_INTERNAL
	ids[n++] = TAB_DEBUG;
#endif
	return n;
}

static const char *ui_tab_name(int id)
{
	return (id == TAB_INFO) ? "INFO" : (id == TAB_PERF) ? "PERF" : "DEBUG";
}

#ifdef CTR_INTERNAL
enum
{
	BTN_STEREO = 0,
	BTN_DRAWMERGE,
	BTN_SDLOG,
	BTN_FLUSH,
	BTN_RTHREAD,
	BTN_FTP,
	BTN_BTMMAP,
	BTN_OLD3DS,
	BTN_COUNT
};

/* Two columns of 34 px tall buttons (>= 24 px touch targets). */
static void ui_btn_rect(int i, int *x, int *y, int *w, int *h)
{
	*x = (i & 1) ? 162 : 4;
	*y = 32 + ((i >> 1) * 38);
	*w = 154;
	*h = 34;
}
#endif

/* ------------------------------------------------------------------ helpers --- */

static const char *ui_onoff(int v)
{
	return v ? "ON" : "OFF";
}

/* x10 fixed point -> "12.3" without floats in the hot path. */
static void ui_fmt10(char *out, size_t cap, unsigned long long x10)
{
	snprintf(out, cap, "%llu.%llu", x10 / 10ULL, x10 % 10ULL);
}

static void ui_fmt_mb(char *out, size_t cap, unsigned long long bytes)
{
	ui_fmt10(out, cap, (bytes * 10ULL) >> 20);
}

static void ui_fmt_ms(char *out, size_t cap, u32 us)
{
	ui_fmt10(out, cap, (u32)(us / 100U));
}

static void ui_kv(int y, const char *label, const char *value, u32 vcol)
{
	const int x = ui_text(6, y, 2, COL_LABEL, label);

	ui_text(x + 12, y, 2, vcol, value);
}

static void ui_ring_stats(u32 *minUs, u32 *maxUs, u32 *avgUs)
{
	unsigned long long sum = 0;
	u32 lo = 0xFFFFFFFFu, hi = 0;
	int i;

	for (i = 0; i < s_ringCount; i++)
	{
		const u32 v = s_ring[i];

		sum += v;
		if (v < lo) { lo = v; }
		if (v > hi) { hi = v; }
	}
	*minUs = (s_ringCount > 0) ? lo : 0;
	*maxUs = hi;
	*avgUs = (s_ringCount > 0) ? (u32)(sum / (unsigned)s_ringCount) : 0;
}

static u32 ui_time_col(u32 us)
{
	return (us <= 34000U) ? COL_GOOD : (us <= 50000U) ? COL_WARN : COL_BAD;
}

/* ------------------------------------------------------------------- tabs --- */

static void ui_draw_info(void)
{
	char a[24], b[24], c[80];
	u32 mn, mx, av;
	int y = 34;
	struct mallinfo mi = mallinfo();
	const int ftp = N3DS_FtpStatus();

	ui_ring_stats(&mn, &mx, &av);

	/* FPS: instantaneous from the last frame, average over the ring. */
	ui_fmt10(a, sizeof(a), (s_lastDtUs > 0) ? (10000000ULL / s_lastDtUs) : 0ULL);
	ui_fmt10(b, sizeof(b), (av > 0) ? (10000000ULL / av) : 0ULL);
	snprintf(c, sizeof(c), "%s  AVG %s", a, b);
	ui_kv(y, "FPS", c, ui_time_col(av));
	y += 17;

	ui_fmt_ms(a, sizeof(a), s_lastDtUs);
	ui_fmt_ms(b, sizeof(b), mx);
	snprintf(c, sizeof(c), "%s WORST %s/%dF", a, b, s_ringCount);
	ui_kv(y, "FT", c, ui_time_col(mx));
	y += 17;

	snprintf(c, sizeof(c), "%u TRI~%u /F", s_drawsPerFrame, s_vertsPerFrame / 3U);
	ui_kv(y, "DRAW", c, COL_TEXT);
	y += 17;

	ui_fmt_mb(a, sizeof(a), N3DS_LinearFreeBytes());
	snprintf(c, sizeof(c), "%sMB", a);
	ui_kv(y, "LINEAR FREE", c, COL_TEXT);
	y += 17;

	ui_fmt_mb(a, sizeof(a), (unsigned long long)osGetMemRegionFree(MEMREGION_APPLICATION));
	ui_fmt_mb(b, sizeof(b), (unsigned long long)osGetMemRegionSize(MEMREGION_APPLICATION));
	snprintf(c, sizeof(c), "%s/%sMB", a, b);
	ui_kv(y, "APP FREE", c, COL_TEXT);
	y += 17;

	ui_fmt_mb(a, sizeof(a), (unsigned long long)mi.uordblks);
	ui_fmt_mb(b, sizeof(b), (unsigned long long)mi.fordblks);
	snprintf(c, sizeof(c), "%sMB FREE %s", a, b);
	ui_kv(y, "HEAP", c, COL_TEXT);
	y += 17;

	if (s_isNew3DS < 0)
	{
		bool n = false;

		APT_CheckNew3DS(&n);
		s_isNew3DS = n ? 1 : 0;
	}
	snprintf(c, sizeof(c), "%s %s  SDLOG %s", s_isNew3DS ? "NEW" : "OLD",
	         (s_isNew3DS && N3DS_SpeedupEnabled()) ? "804MHZ" : "268MHZ", ui_onoff(g_dbg_sdLog));
	ui_kv(y, "3DS", c, (s_isNew3DS && !N3DS_SpeedupEnabled()) ? COL_WARN : COL_TEXT);
	y += 17;

	{
		const float sl = N3DS_StereoSliderState();

		snprintf(c, sizeof(c), "%d.%02d %s %s", (int)sl, (int)(sl * 100.0f) % 100, g_dbg_stereo ? "ON" : "FORCED OFF",
		         N3DS_StereoFrameActive() ? "3D" : "2D");
		ui_kv(y, "SLIDER", c, COL_TEXT);
		y += 17;
	}

	snprintf(c, sizeof(c), "%d MODE %X ST %d", s_gi.levelID, (unsigned)s_gi.gameMode, s_gi.mainState);
	ui_kv(y, "LEVEL", c, COL_TEXT);
	y += 17;

	if (ftp == N3DS_FTP_STATUS_LISTENING)
	{
		struct in_addr ia;

		ia.s_addr = N3DS_FtpAddress();
		snprintf(c, sizeof(c), "%s:%d", inet_ntoa(ia), N3DS_FtpPort());
		ui_kv(y, "FTP", c, COL_GOOD);
	}
	else
	{
		ui_kv(y, "FTP",
		      (ftp == N3DS_FTP_STATUS_OFF)            ? "OFF"
		      : (ftp == N3DS_FTP_STATUS_WAITING_WIFI) ? "ON, WAITING FOR WIFI"
		      : (ftp == N3DS_FTP_STATUS_STOPPING)     ? "STOPPING..."
		                                              : "STUCK, RESTART GAME",
		      (ftp == N3DS_FTP_STATUS_OFF) ? COL_LABEL : COL_WARN);
	}

	ui_text(6, 228, 1, COL_LABEL, "SELECT+DOWN: CLOSE");
}

static void ui_draw_perf(void)
{
	/* 120 bars x 2 px = 240 px wide; 1.5 px per ms, full scale 66.7 ms (2 vblank-pairs). */
	const int gx = 40, gy = 36, gh = 100;
	char a[24], b[24], c[80];
	u32 mn, mx, av;
	int i, y;

	ui_ring_stats(&mn, &mx, &av);

	ui_rect(gx, gy, UI_RING * 2, gh, RGB(0x18, 0x1C, 0x2A));
	for (i = 0; i < s_ringCount; i++)
	{
		/* oldest first */
		const u32 us = s_ring[(s_ringHead - s_ringCount + i + (UI_RING * 2)) % UI_RING];
		int h = (int)((us * 3U) / 2000U); /* us -> px at 1.5 px/ms */

		if (h > gh) { h = gh; }
		if (h < 1) { h = 1; }
		ui_rect(gx + (i * 2), gy + gh - h, 2, h, ui_time_col(us));
	}
	/* 16.7 ms and 33.3 ms guides */
	ui_rect(gx, gy + gh - 25, UI_RING * 2, 1, RGB(0x70, 0x80, 0xA0));
	ui_rect(gx, gy + gh - 50, UI_RING * 2, 1, RGB(0x70, 0x80, 0xA0));
	ui_text(10, gy + gh - 54, 1, COL_LABEL, "33MS");
	ui_text(10, gy + gh - 29, 1, COL_LABEL, "17MS");
	ui_text(10, gy, 1, COL_LABEL, "67MS");

	y = gy + gh + 8;
	ui_fmt_ms(a, sizeof(a), av);
	ui_fmt_ms(b, sizeof(b), mn);
	snprintf(c, sizeof(c), "%s MIN %s", a, b);
	ui_text(6, y, 1, COL_LABEL, "AVG");
	ui_text(6 + 24, y, 2, ui_time_col(av), c);
	y += 16;
	ui_fmt_ms(a, sizeof(a), mx);
	snprintf(c, sizeof(c), "%sMS LAST %d FRAMES", a, s_ringCount);
	ui_text(6, y, 1, COL_LABEL, "MAX");
	ui_text(6 + 24, y, 2, ui_time_col(mx), c);
	y += 20;

#ifdef CTR_INTERNAL
	if (s_gi.perfValid)
	{
		ui_fmt_ms(a, sizeof(a), (u32)s_gi.perfTotalUs);
		ui_fmt_ms(b, sizeof(b), (u32)s_gi.perfWorkUs);
		snprintf(c, sizeof(c), "[PERF] 64F: FRAME %s WORK %s", a, b);
		ui_text(6, y, 1, COL_TEXT, c);
		y += 11;
		ui_fmt_ms(a, sizeof(a), (u32)s_gi.perfWorstUs);
		snprintf(c, sizeof(c), "WORST %s", a);
		ui_text(6, y, 1, COL_TEXT, c);
		y += 11;
		for (i = 0; i < 2; i++)
		{
			if (s_gi.bucketName[i] != NULL)
			{
				ui_fmt_ms(a, sizeof(a), (u32)s_gi.bucketUs[i]);
				snprintf(c, sizeof(c), "%.22s %sMS", s_gi.bucketName[i], a);
				ui_text(6, y, 1, COL_TEXT, c);
				y += 11;
			}
		}
	}
	else
	{
		ui_text(6, y, 1, COL_LABEL, "[PERF] WINDOW: WAITING FOR FIRST 64F");
	}
#else
	(void)y;
#endif
}

#ifdef CTR_INTERNAL
static void ui_btn_label(int i, const char **name, const char **state, u32 *col)
{
	const int ftp = N3DS_FtpStatus();
	int on = -1;

	*col = COL_BTN_ACT;
	switch (i)
	{
	case BTN_STEREO: *name = "STEREO 3D"; on = g_dbg_stereo; break;
	case BTN_DRAWMERGE: *name = "DRAW MERGE"; on = g_dbg_drawMerge; break;
	case BTN_SDLOG: *name = "SD LOG"; on = g_dbg_sdLog; break;
	case BTN_FLUSH: *name = "LOG"; *state = "FLUSH"; return;
	case BTN_RTHREAD: *name = "RENDER THREAD"; on = g_dbg_renderThread; break;
	case BTN_FTP:
		*name = "FTP SERVER";
		on = (ftp == N3DS_FTP_STATUS_LISTENING) || (ftp == N3DS_FTP_STATUS_WAITING_WIFI);
		*state = (ftp == N3DS_FTP_STATUS_STOPPING) ? "STOPPING" : (ftp == N3DS_FTP_STATUS_STUCK) ? "STUCK" : ui_onoff(on);
		*col = on ? COL_BTN_ON : COL_BTN_OFF;
		return;
	case BTN_BTMMAP: *name = "BOTTOM MAP"; on = g_dbg_btmMap; break;
	default:
		*name = "OLD 3DS MODE";
		on = !N3DS_SpeedupEnabled();
		*state = on ? "ON (SLOW)" : "OFF";
		*col = on ? COL_BTN_OFF : COL_BTN_ON;
		return;
	}
	*state = ui_onoff(on);
	*col = on ? COL_BTN_ON : COL_BTN_OFF;
}

static void ui_draw_debug(void)
{
	int i;

	for (i = 0; i < BTN_COUNT; i++)
	{
		int x, y, w, h;
		const char *name = "", *state = "";
		u32 col;

		ui_btn_rect(i, &x, &y, &w, &h);
		ui_btn_label(i, &name, &state, &col);
		ui_rect(x, y, w, h, col);
		ui_frame(x, y, w, h, (i == s_pressBtn) ? 3 : 1, (i == s_pressBtn) ? COL_PRESS : RGB(0x80, 0x88, 0xA0));
		ui_text(x + 6, y + 4, 1, COL_TEXT, name);
		ui_text(x + 6, y + 16, 2, COL_TEXT, state);
	}
	ui_text(6, 186, 1, COL_WARN, "! OLD 3DS MODE TURNS OFF THE 804MHZ");
	ui_text(6, 197, 1, COL_WARN, "  CLOCK-UP: THE GAME WILL RUN SLOWER.");
	ui_text(6, 212, 1, COL_LABEL, "SAME SWITCHES AS DUMPREQ.TXT");
	if (s_msg[0] != '\0')
	{
		ui_text(6, 226, 1, COL_GOOD, s_msg);
	}
}
#endif

static void ui_draw_all(void)
{
	int ids[TAB_COUNT];
	const int n = ui_tab_list(ids);
	const int tw = (UI_W - CLOSE_W) / n;
	int i;

	ui_rect(0, 0, UI_W, UI_H, COL_BG);
	for (i = 0; i < n; i++)
	{
		const char *nm = ui_tab_name(ids[i]);
		const int on = (ids[i] == s_tab);
		const int x = i * tw;

		ui_rect(x, 0, tw - 2, TAB_BAR_H, on ? COL_TAB_ON : COL_TAB_OFF);
		ui_text(x + ((tw - 2 - ui_text_w(2, nm)) / 2), 7, 2, COL_TEXT, nm);
	}
	ui_rect(UI_W - CLOSE_W, 0, CLOSE_W, TAB_BAR_H, RGB(0x90, 0x28, 0x28));
	ui_text(UI_W - CLOSE_W + 10, 7, 2, COL_TEXT, "X");

	switch (s_tab)
	{
	case TAB_PERF: ui_draw_perf(); break;
#ifdef CTR_INTERNAL
	case TAB_DEBUG: ui_draw_debug(); break;
#endif
	default: ui_draw_info(); break;
	}
}

/* ------------------------------------------------------------- open / close --- */

static void ui_clear_fb_black(void)
{
	u16 w = 0, h = 0;
	u8 *fb = gfxGetFramebuffer(GFX_BOTTOM, GFX_LEFT, &w, &h);

	if (fb != NULL)
	{
		memset(fb, 0, (size_t)w * h * 3U);
		GSPGPU_FlushDataCache(fb, (u32)w * h * 3U);
	}
}

void N3DS_DebugUI_Toggle(void)
{
	if (!g_n3dsDebugUiOpen)
	{
		s_tab = TAB_INFO;
		s_ringCount = 0;
		s_ringHead = 0;
		s_lastFrameTick = 0;
		s_lastDrawTick = 0;
		s_prevDraws = s_prevVerts = 0;
		s_msg[0] = '\0';
		s_pressBtn = -1;
		s_dirty = 4;
		/* Single-buffered while open: the UI repaints only when something
		 * changed, which would flicker between two buffers. */
		gfxSetDoubleBuffering(GFX_BOTTOM, false);
		g_n3dsDebugUiOpen = 1;
	}
	else
	{
		int i;

		g_n3dsDebugUiOpen = 0;
		/* Blank both buffers so no UI remnant survives into the map/console. */
		ui_clear_fb_black();
		gfxSetDoubleBuffering(GFX_BOTTOM, true);
		for (i = 0; i < 2; i++)
		{
			ui_clear_fb_black();
			gfxScreenSwapBuffers(GFX_BOTTOM, false);
		}
	}
}

/* ------------------------------------------------------------------- touch --- */

#ifdef CTR_INTERNAL
static void ui_set_msg(const char *m)
{
	snprintf(s_msg, sizeof(s_msg), "%s", m);
}
#endif

#ifdef CTR_INTERNAL
static void ui_press_button(int i)
{
	switch (i)
	{
	case BTN_STEREO: N3DS_QueueDumpReq("stereo"); ui_set_msg("STEREO TOGGLED"); break;
	case BTN_DRAWMERGE: N3DS_QueueDumpReq("drawmerge"); ui_set_msg("DRAW MERGE TOGGLED"); break;
	case BTN_SDLOG: N3DS_QueueDumpReq("sdlog"); ui_set_msg("SD LOG TOGGLED"); break;
	case BTN_RTHREAD: N3DS_QueueDumpReq("renderthread"); ui_set_msg("RENDER THREAD TOGGLED"); break;
	case BTN_BTMMAP: N3DS_QueueDumpReq("btmmap"); ui_set_msg("BOTTOM MAP TOGGLED"); break;
	case BTN_FLUSH:
		N3DS_LogFlushNow();
		ui_set_msg("LOG FLUSHED");
		break;
	case BTN_FTP:
	{
		const int st = N3DS_FtpStatus();

		if ((st == N3DS_FTP_STATUS_STOPPING) || (st == N3DS_FTP_STATUS_STUCK))
		{
			ui_set_msg("FTP BUSY, TRY AGAIN");
		}
		else if (st == N3DS_FTP_STATUS_OFF)
		{
			N3DS_FtpSetEnabled(1);
			ui_set_msg(N3DS_FtpStatus() == N3DS_FTP_STATUS_OFF ? "FTP NOT AVAILABLE (SEE LOG)" : "FTP STARTING");
		}
		else
		{
			N3DS_FtpSetEnabled(0);
			ui_set_msg("FTP STOPPING");
		}
		break;
	}
	case BTN_OLD3DS:
		N3DS_QueueDumpReq(N3DS_SpeedupEnabled() ? "speedoff" : "speedon");
		ui_set_msg("CLOCK MODE CHANGED");
		break;
	default: break;
	}
	s_pressBtn = i;
	s_pressFrames = 8;
}
#endif

void N3DS_DebugUI_Touch(void)
{
	touchPosition t;
	int ids[TAB_COUNT];
	int n, i;

	if (!(hidKeysDown() & KEY_TOUCH))
	{
		return;
	}
	hidTouchRead(&t);
	s_dirty = 3;

	if (t.py < TAB_BAR_H)
	{
		if (t.px >= (UI_W - CLOSE_W))
		{
			N3DS_DebugUI_Toggle();
			return;
		}
		n = ui_tab_list(ids);
		i = t.px / ((UI_W - CLOSE_W) / n);
		if (i < n)
		{
			s_tab = ids[i];
		}
		return;
	}
#ifdef CTR_INTERNAL
	if (s_tab == TAB_DEBUG)
	{
		for (i = 0; i < BTN_COUNT; i++)
		{
			int x, y, w, h;

			ui_btn_rect(i, &x, &y, &w, &h);
			if ((t.px >= x) && (t.px < x + w) && (t.py >= y) && (t.py < y + h))
			{
				ui_press_button(i);
				return;
			}
		}
	}
#endif
}

/* -------------------------------------------------------------------- tick --- */

void N3DS_DebugUI_Tick(const struct N3DS_DebugUIGameInfo *gi)
{
	const u64 now = svcGetSystemTick();
	u16 w = 0, h = 0;

	if (!g_n3dsDebugUiOpen)
	{
		return;
	}
	if (gi != NULL)
	{
		s_gi = *gi;
	}
	N3DS_FtpPoll();

	if (s_lastFrameTick != 0)
	{
		s_lastDtUs = (u32)(((now - s_lastFrameTick) * 1000000ULL) / SYSCLOCK_ARM11);
		s_ring[s_ringHead] = s_lastDtUs;
		s_ringHead = (s_ringHead + 1) % UI_RING;
		if (s_ringCount < UI_RING)
		{
			s_ringCount++;
		}
		if (s_prevDraws != 0)
		{
			s_drawsPerFrame = s_gi.drawCalls - s_prevDraws;
			s_vertsPerFrame = s_gi.mergeVerts - s_prevVerts;
		}
	}
	s_lastFrameTick = now;
	s_prevDraws = s_gi.drawCalls;
	s_prevVerts = s_gi.mergeVerts;

	if (s_pressFrames > 0)
	{
		if (--s_pressFrames == 0)
		{
			s_pressBtn = -1;
			s_dirty = 2;
		}
	}

	if ((s_dirty > 0) || ((now - s_lastDrawTick) >= (UI_REDRAW_US * SYSCLOCK_ARM11 / 1000000ULL)))
	{
		s_fb = gfxGetFramebuffer(GFX_BOTTOM, GFX_LEFT, &w, &h);
		if ((s_fb != NULL) && (w == UI_H) && (h == UI_W))
		{
			ui_draw_all();
			/* N3DS_GfxPresent flushes the bottom framebuffer right after this. */
		}
		if (s_dirty > 0)
		{
			s_dirty--;
		}
		s_lastDrawTick = now;
	}
}
