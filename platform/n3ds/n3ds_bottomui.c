/*
 * Bottom-screen UI (see n3ds_bottomui.h for the ownership model).
 *
 * Why this file exists in this shape -- the failure it replaces. libctru's
 * consoleInit(GFX_BOTTOM) calls gfxSetScreenFormat(RGB565) and
 * gfxSetDoubleBuffering(false) (checked in libctru.a's console.o), so the
 * moment the boot console came up the panel was a 2-byte-per-pixel single
 * buffer. The earlier UI then wrote 3-byte BGR8 pixels into it, toggled double
 * buffering at runtime, and the minimap's display transfer filled the same
 * memory from the GPU: three writers, two formats, hence "everything mixed".
 * Now the format is BGR8 and the buffering double, both set once in
 * N3DS_GfxInit, and the citro3d bottom target presents every frame after the
 * first (n3ds_backend.c, N3DS_BottomPass_*).
 *
 * Panel geometry, for the one CPU blit (boot only): the 3DS framebuffer is
 * stored rotated, 240 pixels per column and 320 columns, BGR8, so panel pixel
 * (x, y from the top) lives at ((x * 240) + (239 - y)) * 3. Everywhere else
 * this file works in an ordinary row-major 320x240 picture and the GPU's
 * display transfer does the rotation.
 *
 * Text is the 5x7 bitmap font of the author's other 3DS ports (sm-3ds ui_font.c
 * / mzm), upper case only; labels follow the console's system language
 * (Spanish, else English), as those ports do.
 *
 * THREADING: input, Frame and the FTP calls run on the game thread; Paint runs
 * on whichever thread ends the frame (the render worker when the queue is
 * live). They share plain ints (tab, flags); Paint clears the dirty flag
 * BEFORE drawing, so a change made meanwhile re-dirties and is drawn next
 * frame. The log ring is the only multi-writer structure (atomic head).
 */

#include <3ds.h>
#include <arpa/inet.h>
#include <malloc.h>
#include <netinet/in.h>
#include <stdio.h>
#include <string.h>

#include "n3ds_backend.h"
#include "n3ds_bottomui.h"
#include "n3ds_ftp.h"
#include "update_manifest.h"
#include "updater.h"

#define UI_W 320
#define UI_H 240
#define UI_RING 120 /* frame-time history */
#define LOG_LINES 32
#define LOG_COLS 52

/* Runtime switches shown/toggled here; each is defined by its owning TU. */
extern int g_dbg_sdLog;
extern int g_dbg_stereo;
extern int g_dbg_btmMap;
#ifdef CTR_INTERNAL
extern int g_dbg_drawMerge;
extern int g_dbg_renderThread;
#endif

/* ------------------------------------------------------------------ state --- */

enum
{
	TAB_STATUS = 0,
	TAB_MAP,
	TAB_OPTIONS,
	TAB_UPDATER,
#ifdef CTR_INTERNAL
	TAB_DEBUG,
#endif
	TAB_COUNT
};

#ifdef CTR_INTERNAL
enum
{
	DBG_PERF = 0,
	DBG_ACTIONS,
	DBG_LOG,
	DBG_PAGES
};
#endif

static int s_es = 0; /* Spanish labels */
#define TR(en, es) (s_es ? (es) : (en))

static volatile int s_tab = TAB_STATUS;
static volatile int s_dirty = 1;
static volatile int s_gpuOwned = 0;
static int s_frameSeen = 0; /* the renderer runs: the CPU never blits again, the GPU pass will present */
static volatile int s_mapLive = 0;
static int s_mapZeroFrames = 1000;
#ifdef CTR_INTERNAL
static int s_dbgPage = DBG_PERF;
#endif
static u64 s_lastFrameTick = 0;
static u64 s_lastLiveTick = 0;
static u32 s_ring[UI_RING]; /* frame times, microseconds */
static int s_ringHead = 0;
static int s_ringCount = 0;
static u32 s_lastDtUs = 0;
static struct N3DS_BottomUiGameInfo s_gi;
static unsigned int s_prevDraws = 0, s_prevVerts = 0;
static unsigned int s_drawsPerFrame = 0, s_vertsPerFrame = 0;
static char s_msg[40];
static int s_pressBtn = -1;
static int s_pressFrames = 0;
static int s_isNew3DS = -1;
static unsigned s_ftpSig = 0;
/* In-game updater page (updater.h). The release notes are word-wrapped once on
 * the game thread when a check finishes (Update_FormatNotes keeps a 12 KB buffer
 * on the stack, too much for the render worker that paints), then only drawn. */
#define NOTE_LINES 384
#define NOTE_ROWS 6
static unsigned s_updRev = 0;
static int s_updState = -1;
static int s_updNew = 0; /* a newer build was found: the tab says so */
static char s_noteRaw[12289];
static char s_noteLines[NOTE_LINES][43];
static unsigned s_noteCount = 0;
static unsigned s_noteScroll = 0;
static unsigned s_frameCount = 0;
static char s_status[48];
static char s_logRing[LOG_LINES][LOG_COLS + 1];
static volatile unsigned s_logHead = 0;

/* ----------------------------------------------------------- pixel raster --- */

/* Row-major panel picture, RGBA5551 (the texture's own format, alpha always 1). */
static u16 s_pix[UI_W * UI_H];
/* The same picture Morton-tiled into the 512x256 texture (zero padding stays zero). */
static u16 s_tiled[N3DS_BOTTOM_UI_TEX_W * N3DS_BOTTOM_UI_TEX_H] __attribute__((aligned(32)));

#define RGB(r, g, b) ((u16)((((r) >> 3) << 11) | (((g) >> 3) << 6) | (((b) >> 3) << 1) | 1))
#define COL_BG RGB(0x10, 0x12, 0x1A)
#define COL_MAPBG RGB(0x08, 0x0A, 0x10)
#define COL_TEXT RGB(0xFF, 0xFF, 0xFF)
#define COL_LABEL RGB(0x9A, 0xA6, 0xC0)
#define COL_GOOD RGB(0x50, 0xE0, 0x80)
#define COL_WARN RGB(0xFF, 0xC0, 0x40)
#define COL_BAD RGB(0xFF, 0x60, 0x60)
#define COL_BAR RGB(0x0A, 0x0C, 0x14)
#define COL_TAB_ON RGB(0x2A, 0x6F, 0xDB)
#define COL_TAB_OFF RGB(0x24, 0x28, 0x36)
#define COL_SEP RGB(0x4A, 0x52, 0x6A)
#define COL_ACCENT RGB(0xFF, 0xD7, 0x00)
#define COL_BTN_ON RGB(0x1C, 0x7A, 0x3A)
#define COL_BTN_OFF RGB(0x5A, 0x24, 0x28)
#define COL_BTN_ACT RGB(0x2C, 0x44, 0x80)
#define COL_PRESS RGB(0xFF, 0xFF, 0x60)
#define COL_EDGE RGB(0x80, 0x88, 0xA0)
#define COL_PANEL RGB(0x18, 0x1C, 0x2A)

static void ui_rect(int x, int y, int w, int h, u16 c)
{
	int row, col;

	if (x < 0) { w += x; x = 0; }
	if (y < 0) { h += y; y = 0; }
	if (x + w > UI_W) { w = UI_W - x; }
	if (y + h > UI_H) { h = UI_H - y; }
	if ((w <= 0) || (h <= 0))
	{
		return;
	}
	for (row = y; row < y + h; row++)
	{
		u16 *p = &s_pix[(row * UI_W) + x];

		for (col = 0; col < w; col++)
		{
			p[col] = c;
		}
	}
}

static void ui_frame(int x, int y, int w, int h, int t, u16 c)
{
	ui_rect(x, y, w, t, c);
	ui_rect(x, y + h - t, w, t, c);
	ui_rect(x, y, t, h, c);
	ui_rect(x + w - t, y, t, h, c);
}

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
static int ui_text(int x, int y, int scale, u16 c, const char *s)
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
					ui_rect(x + (col * scale), y + (row * scale), scale, scale, c);
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

static void ui_text_c(int cx, int y, int scale, u16 c, const char *s)
{
	ui_text(cx - (ui_text_w(scale, s) / 2), y, scale, c, s);
}

/* ------------------------------------------------------------------ layout --- */

#define TAB_H N3DS_BOTTOM_TAB_H
#define CONTENT_Y N3DS_BOTTOM_TAB_H
#define BANNER_H 30 /* install notice strip along the bottom, any tab */

static int ui_tab_list(int *ids)
{
	int n = 0;

	ids[n++] = TAB_STATUS;
	ids[n++] = TAB_MAP;
	ids[n++] = TAB_OPTIONS;
	ids[n++] = TAB_UPDATER;
#ifdef CTR_INTERNAL
	ids[n++] = TAB_DEBUG;
#endif
	return n;
}

/* Short enough for a 53 px tab at scale 1 (<= 8 characters). The full name is
 * the page's own title. There is no ACHIEVEMENTS tab: this port has no
 * achievement system, and a dead tab would only mislead. */
static const char *ui_tab_name(int id)
{
	switch (id)
	{
	case TAB_STATUS: return TR("STATUS", "ESTADO");
	case TAB_MAP: return TR("MAP", "MAPA");
	case TAB_OPTIONS: return TR("OPTIONS", "OPCIONES");
	case TAB_UPDATER: return s_updNew ? TR("UPDATE!", "NUEVA!") : TR("UPDATER", "ACTUALIZ");
	default: return "DEBUG";
	}
}

enum
{
	OPT_BTMMAP = 0,
	OPT_STEREO,
	OPT_FPSCAP,
	OPT_CPU804,
	OPT_SDLOG,
	OPT_COUNT
};

/* Five rows of 34 px at a 37 px pitch: the last ends at 216, clear of the
 * message line at UI_H - 12. */
static void ui_opt_rect(int i, int *x, int *y, int *w, int *h)
{
	*x = 6;
	*y = CONTENT_Y + 6 + (i * 37);
	*w = 308;
	*h = 34;
}

#ifdef CTR_INTERNAL
enum
{
	ACT_DRAWMERGE = 0,
	ACT_RTHREAD,
	ACT_FLUSH,
	ACT_FTP,
	ACT_O3DSSIM,
	ACT_COUNT
};

#define DBG_SUB_Y (CONTENT_Y + 3)
#define DBG_SUB_H 20

static void ui_sub_rect(int i, int *x, int *y, int *w, int *h)
{
	*x = 4 + (i * 104);
	*y = DBG_SUB_Y;
	*w = 100;
	*h = DBG_SUB_H;
}

/* Two columns of tall buttons (>= 24 px touch targets). */
static void ui_act_rect(int i, int *x, int *y, int *w, int *h)
{
	*x = (i & 1) ? 162 : 4;
	*y = CONTENT_Y + 30 + ((i >> 1) * 48);
	*w = 154;
	*h = 44;
}
#endif

/* The Updater page's CANCEL button (install-only FTP server), bottom right. */
static void ui_cancel_rect(int *x, int *y, int *w, int *h)
{
	*x = 232;
	*y = CONTENT_Y + 172;
	*w = 82;
	*h = 26;
}

/* Updater page widgets: channel and auto-check toggles, the action button, the
 * notes viewport with its scroll buttons. */
enum
{
	UPD_CHANNEL = 0,
	UPD_AUTO,
	UPD_ACTION,
	UPD_UP,
	UPD_DOWN,
	UPD_COUNT
};
#define UPD_BTN_BASE 110

static void ui_upd_rect(int i, int *x, int *y, int *w, int *h)
{
	switch (i)
	{
	case UPD_CHANNEL: *x = 6; *y = CONTENT_Y + 22; *w = 151; *h = 22; break;
	case UPD_AUTO: *x = 163; *y = CONTENT_Y + 22; *w = 151; *h = 22; break;
	case UPD_ACTION: *x = 6; *y = CONTENT_Y + 48; *w = 308; *h = 24; break;
	case UPD_UP: *x = 270; *y = CONTENT_Y + 104; *w = 44; *h = 32; break;
	default: *x = 270; *y = CONTENT_Y + 140; *w = 44; *h = 32; break;
	}
}

/* ------------------------------------------------------------------ helpers --- */

static const char *ui_onoff(int v)
{
	return v ? TR("ON", "SI") : "OFF";
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

#ifdef CTR_INTERNAL
static void ui_fmt_ms(char *out, size_t cap, u32 us)
{
	ui_fmt10(out, cap, (u32)(us / 100U));
}
#endif

static void ui_kv(int y, const char *label, const char *value, u16 vcol)
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

static u16 ui_time_col(u32 us)
{
	return (us <= 34000U) ? COL_GOOD : (us <= 50000U) ? COL_WARN : COL_BAD;
}

static void ui_button(int x, int y, int w, int h, u16 body, int pressed)
{
	ui_rect(x, y, w, h, body);
	ui_frame(x, y, w, h, pressed ? 3 : 1, pressed ? COL_PRESS : COL_EDGE);
}

static void ui_ftp_address(char *out, size_t cap)
{
	struct in_addr ia;

	ia.s_addr = N3DS_FtpAddress();
	snprintf(out, cap, "%s:%d", inet_ntoa(ia), N3DS_FtpPort());
}

static const char *ui_ftp_state_text(int st)
{
	return (st == N3DS_FTP_STATUS_OFF)            ? "OFF"
	       : (st == N3DS_FTP_STATUS_WAITING_WIFI) ? TR("WAITING FOR WIFI", "ESPERANDO WIFI")
	       : (st == N3DS_FTP_STATUS_STOPPING)     ? TR("STOPPING...", "DETENIENDO...")
	       : (st == N3DS_FTP_STATUS_STUCK)        ? TR("STUCK, RESTART GAME", "ATASCADO, REINICIA")
	                                              : "OK";
}

/* Everything the Updater page and the install banner show, folded into one
 * number: a change of it (not a timer) is what triggers a repaint. */
static unsigned ui_ftp_signature(void)
{
	unsigned long long done = 0, total = 0;
	const int phase = N3DS_FtpProgress(&done, &total);
	const unsigned pct = (total > 0) ? (unsigned)((done * 100ULL) / total) : 0;

	return ((unsigned)N3DS_FtpStatus()) | ((unsigned)phase << 4) | (pct << 8) | (N3DS_FtpInstallOnly() ? 0x10000u : 0) |
	       (N3DS_FtpAddress() << 17) | (unsigned)(N3DS_FtpNotice() != NULL ? 0x8000u : 0);
}

/* ------------------------------------------------------------------- pages --- */

static void ui_draw_status(void)
{
	char a[24], b[24], c[80];
	u32 mn, mx, av;
	int y = CONTENT_Y + 8;
	const int ftp = N3DS_FtpStatus();

	ui_ring_stats(&mn, &mx, &av);

	ui_fmt10(a, sizeof(a), (s_lastDtUs > 0) ? (10000000ULL / s_lastDtUs) : 0ULL);
	ui_fmt10(b, sizeof(b), (av > 0) ? (10000000ULL / av) : 0ULL);
	snprintf(c, sizeof(c), "%s  AVG %s", a, b);
	ui_kv(y, "FPS", c, ui_time_col(av));
	y += 17;

	ui_fmt_mb(a, sizeof(a), N3DS_LinearFreeBytes());
	snprintf(c, sizeof(c), "%sMB", a);
	ui_kv(y, TR("LINEAR FREE", "LINEAL LIBRE"), c, COL_TEXT);
	y += 17;

	{
		ui_fmt_mb(a, sizeof(a), (unsigned long long)N3DS_HeapUsedBytes());
		ui_fmt_mb(b, sizeof(b), (unsigned long long)N3DS_HeapFreeBytes());
		snprintf(c, sizeof(c), "%sMB / %sMB FREE", a, b);
		ui_kv(y, "HEAP", c, COL_TEXT);
		y += 17;
	}

	if (s_isNew3DS < 0)
	{
		bool n = false;

		APT_CheckNew3DS(&n);
		s_isNew3DS = n ? 1 : 0;
	}
	snprintf(c, sizeof(c), "%s %s%s", s_isNew3DS ? "NEW" : "OLD", (s_isNew3DS && N3DS_SpeedupEnabled()) ? "804MHZ" : "268MHZ",
	         N3DS_O3dsSimActive() ? " SIM" : "");
	ui_kv(y, "3DS", c, (s_isNew3DS && !N3DS_SpeedupEnabled()) ? COL_WARN : COL_TEXT);
	y += 17;

	{
		const float sl = N3DS_StereoSliderState();

		snprintf(c, sizeof(c), "%d.%02d %s", (int)sl, (int)(sl * 100.0f) % 100,
		         !g_dbg_stereo                     ? TR("OFF (OPTIONS)", "NO (OPCIONES)")
		         : N3DS_FramePaceStereoSuppressed() ? TR("2D (AUTO, SLOW)", "2D (AUTO, LENTO)")
		         : N3DS_StereoFrameActive()         ? "3D"
		                                            : "2D");
		ui_kv(y, TR("3D SLIDER", "SLIDER 3D"), c, COL_TEXT);
		y += 17;
	}

	snprintf(c, sizeof(c), "%d  MODE %X", s_gi.levelID, (unsigned)s_gi.gameMode);
	ui_kv(y, TR("LEVEL", "NIVEL"), c, COL_TEXT);
	y += 17;

	if (ftp == N3DS_FTP_STATUS_LISTENING)
	{
		ui_ftp_address(c, sizeof(c));
		ui_kv(y, N3DS_FtpInstallOnly() ? "FTP (INSTALL)" : "FTP", c, COL_GOOD);
	}
	else
	{
		ui_kv(y, "FTP", ui_ftp_state_text(ftp), (ftp == N3DS_FTP_STATUS_OFF) ? COL_LABEL : COL_WARN);
	}
	y += 17;

	if (s_status[0] != '\0')
	{
		ui_text(6, y + 2, 1, COL_WARN, s_status);
	}
	else
	{
		ui_text(6, y + 2, 1, COL_LABEL, "CTR 3D " CTR_NATIVE_VERSION);
	}
}

static void ui_draw_map(void)
{
	ui_rect(0, CONTENT_Y, UI_W, UI_H - CONTENT_Y, COL_MAPBG);
	if (!g_dbg_btmMap)
	{
		ui_text_c(UI_W / 2, 100, 1, COL_WARN, TR("MINIMAP ON THE TOP SCREEN", "MAPA EN LA PANTALLA SUPERIOR"));
		ui_text_c(UI_W / 2, 114, 1, COL_LABEL, TR("TURN IT ON IN OPTIONS", "ACTIVALO EN OPCIONES"));
	}
	else if (!s_mapLive)
	{
		ui_text_c(UI_W / 2, 100, 2, COL_LABEL, TR("NO MAP NOW", "SIN MAPA AHORA"));
		ui_text_c(UI_W / 2, 124, 1, COL_LABEL, TR("IT SHOWS HERE DURING A RACE", "SE VE AQUI EN CARRERA"));
	}
}

static void ui_draw_option(int i, const char *label, int on, const char *stateText)
{
	int x, y, w, h;

	ui_opt_rect(i, &x, &y, &w, &h);
	ui_button(x, y, w, h, on ? COL_BTN_ON : COL_BTN_OFF, (i == s_pressBtn));
	ui_text(x + 10, y + 10, 2, COL_TEXT, label);
	ui_text(x + w - 10 - ui_text_w(2, stateText), y + 10, 2, COL_TEXT, stateText);
}

/* 3D EFFECT: OFF / ON / AUTO (the Old-3DS default: the right eye yields when it
 * alone costs the frame its cap, see n3ds_backend.h's frame-pace block). */
static const char *ui_stereo_text(void)
{
	if (g_dbg_stereo == 2)
	{
		return N3DS_FramePaceStereoSuppressed() ? TR("AUTO (2D)", "AUTO (2D)") : "AUTO";
	}
	return ui_onoff(g_dbg_stereo);
}

/* FRAME CAP: AUTO shows the cap in force. */
static const char *ui_fpscap_text(void)
{
	if (g_dbg_frameCap == 1)
	{
		return "30";
	}
	if (g_dbg_frameCap == 2)
	{
		return "20";
	}
	return (N3DS_FramePaceFlipVBlanks() == 3) ? "AUTO (20)" : "AUTO (30)";
}

static void ui_draw_options(void)
{
	ui_draw_option(OPT_BTMMAP, TR("MINIMAP BELOW", "MAPA ABAJO"), g_dbg_btmMap, ui_onoff(g_dbg_btmMap));
	ui_draw_option(OPT_STEREO, TR("3D EFFECT", "EFECTO 3D"), g_dbg_stereo, ui_stereo_text());
	ui_draw_option(OPT_FPSCAP, TR("FRAME CAP", "LIMITE FPS"), 1, ui_fpscap_text());
	if (s_isNew3DS < 0)
	{
		bool n = false;

		APT_CheckNew3DS(&n);
		s_isNew3DS = n ? 1 : 0;
	}
	if (s_isNew3DS)
	{
		ui_draw_option(OPT_CPU804, "CPU 804MHZ", N3DS_SpeedupEnabled(), ui_onoff(N3DS_SpeedupEnabled()));
	}
	else
	{
		/* Old 3DS: there is no clock-up to switch; say so instead of a dead button. */
		int x, y, w, h;

		ui_opt_rect(OPT_CPU804, &x, &y, &w, &h);
		ui_rect(x, y, w, h, COL_PANEL);
		ui_text(x + 10, y + 10, 2, COL_LABEL, "OLD 3DS: 268MHZ");
	}
	ui_draw_option(OPT_SDLOG, TR("SD LOG", "LOG EN SD"), g_dbg_sdLog, ui_onoff(g_dbg_sdLog));
	if (s_msg[0] != '\0')
	{
		ui_text(6, UI_H - 12, 1, COL_GOOD, s_msg);
	}
}

static void ui_draw_bar(int x, int y, int w, int h, unsigned long long done, unsigned long long total, u16 fill)
{
	ui_frame(x, y, w, h, 1, COL_EDGE);
	ui_rect(x + 1, y + 1, w - 2, h - 2, COL_PANEL);
	if (total > 0)
	{
		const int fw = (int)(((unsigned long long)(w - 2) * (done > total ? total : done)) / total);

		ui_rect(x + 1, y + 1, fw, h - 2, fill);
	}
}

static void ui_draw_updater(void)
{
	char c[96];
	UpdateStatus st;
	int x, y, w, h, i;
	const int busy = Updater_Busy();
	const int ftp = N3DS_FtpStatus();

	Updater_GetStatus(&st);
	ui_text(6, CONTENT_Y + 4, 2, COL_ACCENT, TR("UPDATER", "ACTUALIZADOR"));
	ui_text(314 - ui_text_w(1, CTR_NATIVE_VERSION), CONTENT_Y + 8, 1, COL_LABEL, CTR_NATIVE_VERSION);

	/* Channel and auto-check toggles. */
	ui_upd_rect(UPD_CHANNEL, &x, &y, &w, &h);
	ui_button(x, y, w, h, busy ? COL_PANEL : COL_BTN_ACT, (s_pressBtn == UPD_BTN_BASE + UPD_CHANNEL));
	ui_text_c(x + (w / 2), y + 7, 1, COL_TEXT, st.betas ? TR("CHANNEL: + BETAS", "CANAL: + BETAS") : TR("CHANNEL: STABLE", "CANAL: ESTABLE"));
	ui_upd_rect(UPD_AUTO, &x, &y, &w, &h);
	{
		const int on = Updater_AutoCheckEnabled();

		ui_button(x, y, w, h, on ? COL_BTN_ON : COL_BTN_OFF, (s_pressBtn == UPD_BTN_BASE + UPD_AUTO));
		snprintf(c, sizeof(c), "%s %s", TR("AUTO CHECK:", "AUTO BUSCAR:"), ui_onoff(on));
		ui_text_c(x + (w / 2), y + 7, 1, COL_TEXT, c);
	}

	/* The action button follows the state. */
	{
		const char *label = TR("CHECK FOR UPDATES", "BUSCAR ACTUALIZACION");
		u16 col = COL_BTN_ACT;

		if (st.state == UPDATE_CHECKING)
		{
			label = TR("CHECKING...", "BUSCANDO...");
			col = COL_PANEL;
		}
		else if (st.state == UPDATE_AVAILABLE)
		{
			label = TR("DOWNLOAD AND INSTALL", "DESCARGAR E INSTALAR");
			col = COL_BTN_ON;
		}
		else if (st.state == UPDATE_DOWNLOADING)
		{
			label = TR("CANCEL DOWNLOAD", "CANCELAR DESCARGA");
			col = COL_BTN_OFF;
		}
		else if ((st.state == UPDATE_VERIFYING) || (st.state == UPDATE_INSTALLING))
		{
			label = TR("INSTALLING...", "INSTALANDO...");
			col = COL_PANEL;
		}
		else if (st.state == UPDATE_DONE)
		{
			label = TR("RESTARTING...", "REINICIANDO...");
			col = COL_PANEL;
		}
		ui_upd_rect(UPD_ACTION, &x, &y, &w, &h);
		ui_button(x, y, w, h, col, (s_pressBtn == UPD_BTN_BASE + UPD_ACTION));
		ui_text_c(x + (w / 2), y + 5, 2, COL_TEXT, label);
	}

	/* Status line, found version and progress. */
	y = CONTENT_Y + 78;
	{
		u16 col = (st.state == UPDATE_ERROR) ? COL_BAD : (st.state == UPDATE_AVAILABLE || st.state == UPDATE_DONE) ? COL_GOOD : COL_TEXT;

		ui_text(6, y, 1, col, st.message[0] ? st.message : TR("PRESS CHECK FOR UPDATES", "PULSA BUSCAR ACTUALIZACION"));
		if (st.version[0] != '\0')
		{
			snprintf(c, sizeof(c), "%s", st.version);
			ui_text(314 - ui_text_w(1, c), y, 1, COL_ACCENT, c);
		}
	}
	if ((st.state == UPDATE_DOWNLOADING) || (st.state == UPDATE_VERIFYING) || (st.state == UPDATE_INSTALLING))
	{
		ui_draw_bar(6, CONTENT_Y + 90, 308, 9, st.progress, 100, COL_GOOD);
	}
	else if (Updater_CannotInstall())
	{
		ui_text(6, CONTENT_Y + 91, 1, COL_WARN, TR("RUN THE INSTALLED CIA TO UPDATE", "USA EL CIA INSTALADO PARA ACTUALIZAR"));
	}

	/* Release notes (the newer releases of the channel, or the whole changelog when up to date). */
	if (s_noteCount > 0)
	{
		for (i = 0; i < NOTE_ROWS; i++)
		{
			const unsigned line = s_noteScroll + (unsigned)i;

			if (line < s_noteCount)
			{
				ui_text(6, CONTENT_Y + 104 + (i * 11), 1, COL_LABEL, s_noteLines[line]);
			}
		}
		if (s_noteCount > NOTE_ROWS)
		{
			ui_upd_rect(UPD_UP, &x, &y, &w, &h);
			ui_button(x, y, w, h, COL_TAB_OFF, (s_pressBtn == UPD_BTN_BASE + UPD_UP));
			ui_text_c(x + (w / 2), y + 12, 1, COL_TEXT, "UP");
			ui_upd_rect(UPD_DOWN, &x, &y, &w, &h);
			ui_button(x, y, w, h, COL_TAB_OFF, (s_pressBtn == UPD_BTN_BASE + UPD_DOWN));
			ui_text_c(x + (w / 2), y + 12, 1, COL_TEXT, "DOWN");
		}
	}

	/* The FTP side (developers): address while it listens, CANCEL in install-only mode. */
	y = CONTENT_Y + 172;
	if (ftp == N3DS_FTP_STATUS_LISTENING)
	{
		char a[48];

		ui_ftp_address(a, sizeof(a));
		snprintf(c, sizeof(c), "%s %s", N3DS_FtpInstallOnly() ? TR("INSTALL SERVER", "SERVIDOR INSTALACION") : "FTP", a);
		ui_text(6, y, 1, COL_GOOD, c);
		ui_text(6, y + 11, 1, COL_LABEL, TR("SEND A .CIA, THEN SITE INSTALL <PATH>", "ENVIA UN .CIA Y SITE INSTALL <RUTA>"));
		if (N3DS_FtpInstallOnly())
		{
			ui_cancel_rect(&x, &y, &w, &h);
			ui_button(x, y, w, h, COL_BTN_OFF, (s_pressBtn == 100));
			ui_text_c(x + (w / 2), y + 9, 1, COL_TEXT, TR("CANCEL FTP", "CANCELAR FTP"));
		}
	}
	else if (ftp == N3DS_FTP_STATUS_OFF)
	{
		ui_text(6, y, 1, COL_LABEL, TR("DEV INSTALL OVER FTP: PUT AN EMPTY", "INSTALAR POR FTP: PON UN"));
		ui_text(6, y + 11, 1, COL_LABEL, TR("dev-ftp.txt IN SDMC:/3DS/CTR_NATIVE/", "dev-ftp.txt EN SDMC:/3DS/CTR_NATIVE/"));
	}
	else
	{
		ui_kv(y, "FTP", ui_ftp_state_text(ftp), COL_WARN);
	}
}

#ifdef CTR_INTERNAL
static void ui_draw_dbg_perf(void)
{
	/* 120 bars x 2 px = 240 px wide; 0.9 px per ms, full scale 66 ms. */
	const int gx = 40, gy = CONTENT_Y + 30, gh = 60;
	char a[24], b[24], c[80];
	u32 mn, mx, av;
	int i, y;

	ui_ring_stats(&mn, &mx, &av);

	ui_rect(gx, gy, UI_RING * 2, gh, COL_PANEL);
	for (i = 0; i < s_ringCount; i++)
	{
		const u32 us = s_ring[(s_ringHead - s_ringCount + i + (UI_RING * 2)) % UI_RING]; /* oldest first */
		int h = (int)((us * 9U) / 10000U);

		if (h > gh) { h = gh; }
		if (h < 1) { h = 1; }
		ui_rect(gx + (i * 2), gy + gh - h, 2, h, ui_time_col(us));
	}
	ui_rect(gx, gy + gh - 15, UI_RING * 2, 1, COL_SEP); /* 16.7 ms */
	ui_rect(gx, gy + gh - 30, UI_RING * 2, 1, COL_SEP); /* 33.3 ms */
	ui_text(8, gy + gh - 33, 1, COL_LABEL, "33MS");
	ui_text(8, gy + gh - 18, 1, COL_LABEL, "17MS");

	y = gy + gh + 6;
	ui_fmt10(a, sizeof(a), (av > 0) ? (10000000ULL / av) : 0ULL);
	ui_fmt_ms(b, sizeof(b), av);
	snprintf(c, sizeof(c), "%s FPS  %sMS", a, b);
	ui_text(6, y, 2, ui_time_col(av), c);
	y += 16;
	ui_fmt_ms(a, sizeof(a), mx);
	ui_fmt_ms(b, sizeof(b), mn);
	snprintf(c, sizeof(c), "MIN %s MAX %s /%dF", b, a, s_ringCount);
	ui_text(6, y, 1, COL_TEXT, c);
	y += 11;
	snprintf(c, sizeof(c), "%u DRAW  TRI~%u /F", s_drawsPerFrame, s_vertsPerFrame / 3U);
	ui_text(6, y, 1, COL_TEXT, c);
	y += 11;
	ui_fmt_mb(a, sizeof(a), (unsigned long long)osGetMemRegionFree(MEMREGION_APPLICATION));
	ui_fmt_mb(b, sizeof(b), (unsigned long long)osGetMemRegionSize(MEMREGION_APPLICATION));
	snprintf(c, sizeof(c), "APP FREE %s/%sMB", a, b);
	ui_text(6, y, 1, COL_TEXT, c);
	y += 11;
	if (s_gi.perfValid)
	{
		ui_fmt_ms(a, sizeof(a), (u32)s_gi.perfTotalUs);
		ui_fmt_ms(b, sizeof(b), (u32)s_gi.perfWorkUs);
		snprintf(c, sizeof(c), "[PERF] 64F FRAME %s WORK %s", a, b);
		ui_text(6, y, 1, COL_TEXT, c);
		y += 11;
		for (i = 0; i < 2; i++)
		{
			if (s_gi.bucketName[i] != NULL)
			{
				ui_fmt_ms(a, sizeof(a), (u32)s_gi.bucketUs[i]);
				snprintf(c, sizeof(c), "%.22s %sMS", s_gi.bucketName[i], a);
				ui_text(6, y, 1, COL_LABEL, c);
				y += 11;
			}
		}
	}
}

static void ui_act_label(int i, const char **name, const char **state, u16 *col)
{
	const int ftp = N3DS_FtpStatus();
	int on = -1;

	*col = COL_BTN_ACT;
	switch (i)
	{
	case ACT_DRAWMERGE: *name = "DRAW MERGE"; on = g_dbg_drawMerge; break;
	case ACT_RTHREAD: *name = "RENDER THREAD"; on = g_dbg_renderThread; break;
	case ACT_FLUSH: *name = "LOG"; *state = "FLUSH"; return;
	case ACT_O3DSSIM:
		*name = "OLD 3DS SIM";
		on = (N3DS_O3dsSimActive() != 0);
		*state = ui_onoff(on);
		*col = on ? COL_BTN_ON : COL_BTN_OFF;
		return;
	default:
		*name = "FTP";
		on = (ftp == N3DS_FTP_STATUS_LISTENING) || (ftp == N3DS_FTP_STATUS_WAITING_WIFI);
		*state = (ftp == N3DS_FTP_STATUS_STOPPING) ? "STOPPING" : (ftp == N3DS_FTP_STATUS_STUCK) ? "STUCK" : ui_onoff(on);
		*col = on ? COL_BTN_ON : COL_BTN_OFF;
		return;
	}
	*state = ui_onoff(on);
	*col = on ? COL_BTN_ON : COL_BTN_OFF;
}

static void ui_draw_dbg_actions(void)
{
	int i;

	for (i = 0; i < ACT_COUNT; i++)
	{
		int x, y, w, h;
		const char *name = "", *state = "";
		u16 col;

		ui_act_rect(i, &x, &y, &w, &h);
		ui_act_label(i, &name, &state, &col);
		ui_button(x, y, w, h, col, (100 + i == s_pressBtn));
		ui_text(x + 6, y + 5, 1, COL_TEXT, name);
		ui_text(x + 6, y + 22, 2, COL_TEXT, state);
	}
	ui_text(6, CONTENT_Y + 174, 1, COL_LABEL, TR("SAME SWITCHES AS DUMPREQ.TXT", "LOS MISMOS INTERRUPTORES QUE DUMPREQ.TXT"));
	if (N3DS_FtpStatus() == N3DS_FTP_STATUS_LISTENING)
	{
		char c[48];

		ui_ftp_address(c, sizeof(c));
		ui_text(6, CONTENT_Y + 184, 1, COL_GOOD, c);
	}
	if (s_msg[0] != '\0')
	{
		ui_text(6, CONTENT_Y + 194, 1, COL_GOOD, s_msg);
	}
}

static void ui_draw_dbg_log(void)
{
	const unsigned head = s_logHead;
	const int rows = (UI_H - (CONTENT_Y + 30) - 4) / 10;
	int i;

	for (i = 0; i < rows; i++)
	{
		const int back = rows - 1 - i; /* newest at the bottom */

		if (head > (unsigned)back)
		{
			const char *line = s_logRing[(head - 1U - (unsigned)back) % LOG_LINES];

			ui_text(4, CONTENT_Y + 30 + (i * 10), 1, (strstr(line, "FAIL") || strstr(line, "fail")) ? COL_BAD : COL_TEXT, line);
		}
	}
}

static void ui_draw_debug(void)
{
	int i;

	for (i = 0; i < DBG_PAGES; i++)
	{
		int x, y, w, h;
		const char *nm = (i == DBG_PERF) ? "PERF" : (i == DBG_ACTIONS) ? TR("ACTIONS", "ACCIONES") : "LOG";

		ui_sub_rect(i, &x, &y, &w, &h);
		ui_button(x, y, w, h, (i == s_dbgPage) ? COL_TAB_ON : COL_TAB_OFF, 0);
		ui_text_c(x + (w / 2), y + 7, 1, COL_TEXT, nm);
	}
	switch (s_dbgPage)
	{
	case DBG_ACTIONS: ui_draw_dbg_actions(); break;
	case DBG_LOG: ui_draw_dbg_log(); break;
	default: ui_draw_dbg_perf(); break;
	}
}
#endif

/* The install notice, on every tab: an install never happens without a warning. */
static void ui_draw_banner(void)
{
	unsigned long long done = 0, total = 0;
	const char *notice = N3DS_FtpNotice();
	const int phase = N3DS_FtpProgress(&done, &total);
	char c[64];
	const int y = UI_H - BANNER_H;

	if (notice == NULL)
	{
		return;
	}
	ui_rect(0, y, UI_W, BANNER_H, RGB(0x30, 0x20, 0x08));
	ui_rect(0, y, UI_W, 2, COL_WARN);
	if (phase == 3)
	{
		snprintf(c, sizeof(c), "%s %u S", notice, (unsigned)((total > done ? total - done : 0) / 1000ULL) + 1U);
	}
	else if (total > 0)
	{
		snprintf(c, sizeof(c), "%s %u%%", notice, (unsigned)((done * 100ULL) / total));
	}
	else
	{
		snprintf(c, sizeof(c), "%s", notice);
	}
	ui_text(6, y + 5, 1, COL_TEXT, c);
	ui_draw_bar(6, y + 17, UI_W - 12, 9, done, total, COL_WARN);
}

static void ui_draw_all(void)
{
	int ids[TAB_COUNT];
	const int n = ui_tab_list(ids);
	const int tw = UI_W / n;
	const int tab = s_tab;
	int i;

	ui_rect(0, 0, UI_W, UI_H, COL_BG);
	/* Tab bar: a dark strip, one block per tab with a gap, the selected one
	 * lit with a gold underline, separators between, and a rule under the bar. */
	ui_rect(0, 0, UI_W, TAB_H, COL_BAR);
	for (i = 0; i < n; i++)
	{
		const char *nm = ui_tab_name(ids[i]);
		const int on = (ids[i] == tab);
		const int x = i * tw;
		const int w = (i == n - 1) ? (UI_W - x) : tw;

		ui_rect(x + 1, 1, w - 2, TAB_H - 4, on ? COL_TAB_ON : COL_TAB_OFF);
		if (on)
		{
			ui_rect(x + 1, TAB_H - 5, w - 2, 3, COL_ACCENT);
		}
		if (i > 0)
		{
			ui_rect(x, 0, 1, TAB_H - 2, COL_SEP);
		}
		ui_text_c(x + (w / 2), 8, 1, on ? COL_TEXT : COL_LABEL, nm);
	}
	ui_rect(0, TAB_H - 2, UI_W, 2, COL_SEP);

	switch (tab)
	{
	case TAB_MAP: ui_draw_map(); break;
	case TAB_OPTIONS: ui_draw_options(); break;
	case TAB_UPDATER: ui_draw_updater(); break;
#ifdef CTR_INTERNAL
	case TAB_DEBUG: ui_draw_debug(); break;
#endif
	default: ui_draw_status(); break;
	}
	ui_draw_banner();
}

/* ------------------------------------------------- picture -> texture/panel --- */

/* PICA200 texture order: 8x8 tiles row-major across the full texture width,
 * Morton inside a tile (x bits even, y bits odd) -- the same addressing as
 * N3DS_TileVRAMDisplayChunk. Row 0 of the picture is memory row 0 (the top of
 * the image samples at V = 2048 in the quad, see N3DS_BottomUiDrawQuad). */
static void ui_tile(void)
{
	static u8 morton[64];
	static int ready = 0;
	int x, y;

	if (!ready)
	{
		for (y = 0; y < 8; y++)
		{
			for (x = 0; x < 8; x++)
			{
				morton[(y * 8) + x] = (u8)((x & 1) | ((y & 1) << 1) | ((x & 2) << 1) | ((y & 2) << 2) | ((x & 4) << 2) | ((y & 4) << 3));
			}
		}
		ready = 1;
	}
	for (y = 0; y < UI_H; y++)
	{
		const u16 *src = &s_pix[y * UI_W];
		const int tileRow = (y >> 3) * (N3DS_BOTTOM_UI_TEX_W / 8);

		for (x = 0; x < UI_W; x++)
		{
			s_tiled[((tileRow + (x >> 3)) * 64) + morton[((y & 7) * 8) + (x & 7)]] = src[x];
		}
	}
}

/* Boot only, before the first GPU frame: the picture straight into the
 * framebuffer, BGR8, column-major (see the header comment). */
static void ui_blit_to_panel(void)
{
	u16 w = 0, h = 0;
	u8 *fb = gfxGetFramebuffer(GFX_BOTTOM, GFX_LEFT, &w, &h);
	int x, y;

	if ((fb == NULL) || (w != UI_H) || (h != UI_W))
	{
		return;
	}
	for (x = 0; x < UI_W; x++)
	{
		u8 *dst = fb + ((size_t)x * UI_H * 3U) + ((UI_H - 1) * 3U);

		for (y = 0; y < UI_H; y++)
		{
			const u16 p = s_pix[(y * UI_W) + x];
			const u32 r = (p >> 11) & 31U, g = (p >> 6) & 31U, b = (p >> 1) & 31U;

			dst[0] = (u8)((b << 3) | (b >> 2));
			dst[1] = (u8)((g << 3) | (g >> 2));
			dst[2] = (u8)((r << 3) | (r >> 2));
			dst -= 3;
		}
	}
	GSPGPU_FlushDataCache(fb, (u32)w * h * 3U);
	gfxScreenSwapBuffers(GFX_BOTTOM, false);
}

/* Pre-GPU only: repaints and blits; the swap exposes it, and the next call
 * fills the other buffer (a repaint is a full picture, so both converge). */
static void ui_cpu_present(void)
{
	if (s_gpuOwned || s_frameSeen || !s_dirty)
	{
		return;
	}
	s_dirty = 0;
	ui_draw_all();
	ui_blit_to_panel();
}

/* ------------------------------------------------------------------- touch --- */

static void ui_set_msg(const char *m)
{
	snprintf(s_msg, sizeof(s_msg), "%s", m);
}

static void ui_press_option(int i)
{
	switch (i)
	{
	case OPT_BTMMAP:
		g_dbg_btmMap ^= 1;
		N3DS_ConfigSet("btmmap", g_dbg_btmMap);
		break;
	case OPT_STEREO:
		/* Old 3DS / 2DS (and the simulation): AUTO -> ON -> OFF -> AUTO. A New
		 * 3DS keeps its two states (a 2 left by the simulation reads as ON). */
		if (N3DS_FramePaceOldClass())
		{
			g_dbg_stereo = (g_dbg_stereo == 2) ? 1 : (g_dbg_stereo == 1) ? 0 : 2;
		}
		else
		{
			g_dbg_stereo = g_dbg_stereo ? 0 : 1;
		}
		N3DS_ConfigSet("stereo", g_dbg_stereo);
		break;
	case OPT_FPSCAP:
		/* AUTO -> 30 -> 20 -> AUTO. */
		g_dbg_frameCap = (g_dbg_frameCap + 1) % 3;
		N3DS_ConfigSet("fpscap", g_dbg_frameCap);
		break;
	case OPT_CPU804:
		if (s_isNew3DS > 0)
		{
			N3DS_SetSpeedup(!N3DS_SpeedupEnabled());
			N3DS_ConfigSet("cpu804", N3DS_SpeedupEnabled());
			ui_set_msg(N3DS_SpeedupEnabled() ? TR("CPU 804MHZ", "CPU 804MHZ") : TR("OLD 3DS SPEED: SLOWER", "VELOCIDAD OLD 3DS: MAS LENTO"));
		}
		break;
	default:
		N3DS_SetSdLogEnabled(!g_dbg_sdLog);
		break;
	}
	s_pressBtn = i;
	s_pressFrames = 8;
}

static void ui_press_updater(int i)
{
	UpdateStatus st;

	Updater_GetStatus(&st);
	switch (i)
	{
	case UPD_CHANNEL:
		if (!Updater_Busy())
		{
			Updater_SetBetas(!st.betas); /* saves the choice and checks again */
		}
		break;
	case UPD_AUTO:
		Updater_SetAutoCheck(!Updater_AutoCheckEnabled());
		break;
	case UPD_ACTION:
		if (st.state == UPDATE_AVAILABLE)
		{
			Updater_Download();
		}
		else if (st.state == UPDATE_DOWNLOADING)
		{
			Updater_Cancel(); /* later steps (verify, install) are not interrupted */
		}
		else if (!Updater_Busy() && (st.state != UPDATE_DONE))
		{
			Updater_Check();
		}
		break;
	case UPD_UP:
		s_noteScroll = (s_noteScroll > 3U) ? (s_noteScroll - 3U) : 0U;
		break;
	default:
		if (s_noteCount > NOTE_ROWS)
		{
			s_noteScroll = (s_noteScroll + 3U <= s_noteCount - NOTE_ROWS) ? (s_noteScroll + 3U) : (s_noteCount - NOTE_ROWS);
		}
		break;
	}
	s_pressBtn = UPD_BTN_BASE + i;
	s_pressFrames = 8;
}

#ifdef CTR_INTERNAL
static void ui_press_action(int i)
{
	switch (i)
	{
	case ACT_DRAWMERGE: N3DS_QueueDumpReq("drawmerge"); ui_set_msg("DRAW MERGE TOGGLED"); break;
	case ACT_RTHREAD: N3DS_QueueDumpReq("renderthread"); ui_set_msg("RENDER THREAD TOGGLED"); break;
	case ACT_FLUSH:
		N3DS_LogFlushNow();
		ui_set_msg("LOG FLUSHED");
		break;
	case ACT_O3DSSIM:
		if (N3DS_O3dsSimToggleFile() == 2)
		{
			ui_set_msg("LEGACY-MODE BUILD: ALWAYS SIM");
		}
		else
		{
			ui_set_msg(N3DS_O3dsSimActive() ? "OLD 3DS SIM: 268MHZ NOW, RESTART FOR MEMORY+CORES" : "OLD 3DS SIM OFF, RESTART");
		}
		break;
	default:
	{
		const int st = N3DS_FtpStatus();

		if ((st == N3DS_FTP_STATUS_STOPPING) || (st == N3DS_FTP_STATUS_STUCK))
		{
			ui_set_msg("FTP BUSY, TRY AGAIN");
		}
		else if (st == N3DS_FTP_STATUS_OFF)
		{
			N3DS_FtpSetEnabled(1);
			N3DS_ConfigSet("ftp", 1);
			ui_set_msg(N3DS_FtpStatus() == N3DS_FTP_STATUS_OFF ? "FTP NOT AVAILABLE (SEE LOG)" : "FTP STARTING");
		}
		else
		{
			N3DS_FtpSetEnabled(0);
			N3DS_ConfigSet("ftp", 0);
			ui_set_msg("FTP STOPPING");
		}
		break;
	}
	}
	s_pressBtn = 100 + i;
	s_pressFrames = 8;
}
#endif

static int ui_in(int px, int py, int x, int y, int w, int h)
{
	return (px >= x) && (px < x + w) && (py >= y) && (py < y + h);
}

void N3DS_BottomUi_Touch(void)
{
	touchPosition t;
	int ids[TAB_COUNT];
	int n, i;

	if (!(hidKeysDown() & KEY_TOUCH))
	{
		return;
	}
	hidTouchRead(&t);

	if (t.py < TAB_H)
	{
		n = ui_tab_list(ids);
		i = t.px / (UI_W / n);
		if (i >= n)
		{
			i = n - 1;
		}
		s_tab = ids[i];
		s_msg[0] = '\0';
		s_dirty = 1;
		return;
	}
	switch (s_tab)
	{
	case TAB_OPTIONS:
		for (i = 0; i < OPT_COUNT; i++)
		{
			int x, y, w, h;

			ui_opt_rect(i, &x, &y, &w, &h);
			if (ui_in(t.px, t.py, x, y, w, h))
			{
				ui_press_option(i);
				s_dirty = 1;
				return;
			}
		}
		break;
	case TAB_UPDATER:
		for (i = 0; i < UPD_COUNT; i++)
		{
			int x, y, w, h;

			ui_upd_rect(i, &x, &y, &w, &h);
			if (ui_in(t.px, t.py, x, y, w, h))
			{
				ui_press_updater(i);
				s_dirty = 1;
				return;
			}
		}
		if (N3DS_FtpInstallOnly())
		{
			int x, y, w, h;

			ui_cancel_rect(&x, &y, &w, &h);
			if (ui_in(t.px, t.py, x, y, w, h))
			{
				N3DS_FtpCancel();
				s_pressBtn = 100;
				s_pressFrames = 8;
				s_dirty = 1;
			}
		}
		break;
#ifdef CTR_INTERNAL
	case TAB_DEBUG:
		for (i = 0; i < DBG_PAGES; i++)
		{
			int x, y, w, h;

			ui_sub_rect(i, &x, &y, &w, &h);
			if (ui_in(t.px, t.py, x, y, w, h))
			{
				s_dbgPage = i;
				s_dirty = 1;
				return;
			}
		}
		if (s_dbgPage == DBG_ACTIONS)
		{
			for (i = 0; i < ACT_COUNT; i++)
			{
				int x, y, w, h;

				ui_act_rect(i, &x, &y, &w, &h);
				if (ui_in(t.px, t.py, x, y, w, h))
				{
					ui_press_action(i);
					s_dirty = 1;
					return;
				}
			}
		}
		break;
#endif
	default:
		break;
	}
}

/* ------------------------------------------------------------- frame / API --- */

void N3DS_BottomUi_Init(void)
{
	u8 lang = CFG_LANGUAGE_EN;

	/* THE one place the panel's format and buffering are decided. consoleInit
	 * would have made it RGB565 + single buffered (see the header); nothing
	 * calls it on this screen any more, and nothing may change these later. */
	gfxSetScreenFormat(GFX_BOTTOM, GSP_BGR8_OES);
	gfxSetDoubleBuffering(GFX_BOTTOM, true);

	if (R_SUCCEEDED(cfguInit()))
	{
		CFGU_GetSystemLanguage(&lang);
		cfguExit();
	}
	s_es = (lang == CFG_LANGUAGE_ES);

	s_dirty = 1;
	/* Both buffers get the first picture, so the first swap shows it whichever
	 * one the display is on. */
	ui_cpu_present();
	s_dirty = 1;
	ui_cpu_present();
}

/* Game thread, once per frame: repaint when the updater's status changed, load the
 * release notes when a check ends, run the check the player asked for at boot. */
static void ui_updater_poll(void)
{
	UpdateStatus st;

	if (++s_frameCount == 300U)
	{
		Updater_AutoCheck();
	}
	Updater_GetStatus(&st);
	if (st.revision != s_updRev)
	{
		s_updRev = st.revision;
		s_dirty = 1;
	}
	if ((int)st.state != s_updState)
	{
		s_updState = (int)st.state;
		s_updNew = (st.state == UPDATE_AVAILABLE);
		if ((st.state == UPDATE_AVAILABLE) || (st.state == UPDATE_CURRENT))
		{
			(void)Updater_GetNotes(s_noteRaw, sizeof(s_noteRaw));
			s_noteCount = Update_FormatNotes(s_noteRaw, s_noteLines, NOTE_LINES);
			s_noteScroll = 0;
		}
		else if (st.state == UPDATE_CHECKING)
		{
			s_noteCount = 0;
			s_noteScroll = 0;
		}
		s_dirty = 1;
	}
}

void N3DS_BottomUi_Frame(const struct N3DS_BottomUiGameInfo *gi)
{
	const u64 now = svcGetSystemTick();
	const u64 liveTicks = (u64)SYSCLOCK_ARM11 / 4ULL; /* 250 ms */
	unsigned sig;
	int tab = s_tab;

	if (!s_frameSeen)
	{
		/* The renderer is up: from here the GPU pass presents the panel. Make
		 * sure it has a picture to take over with. */
		s_frameSeen = 1;
		s_dirty = 1;
	}
	if (gi != NULL)
	{
		s_gi = *gi;
	}
	N3DS_FtpPoll();
	ui_updater_poll();

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

	if ((s_pressFrames > 0) && (--s_pressFrames == 0))
	{
		s_pressBtn = -1;
		s_dirty = 1;
	}

	/* Repaint triggers beyond input: FTP/install state changes (compared, not
	 * timed), and a slow tick for the live numbers of the tab that shows them.
	 * Idle tabs (MAPA, OPCIONES, UPDATER without an install) cost nothing. */
	sig = ui_ftp_signature();
	if (sig != s_ftpSig)
	{
		s_ftpSig = sig;
		s_dirty = 1;
		if (N3DS_FtpNotice() != NULL)
		{
			s_tab = TAB_UPDATER; /* an upload/install has begun: show where */
		}
	}
	{
		/* The frame cap and AUTO 3D change on their own (OPTIONS shows both). */
		static int s_paceSig = -1;
		const int paceSig = (N3DS_FramePaceFlipVBlanks() * 2) + N3DS_FramePaceStereoSuppressed();

		if (paceSig != s_paceSig)
		{
			s_paceSig = paceSig;
			s_dirty = 1;
		}
	}
	if ((now - s_lastLiveTick) >= liveTicks)
	{
		int live = (tab == TAB_STATUS);

#ifdef CTR_INTERNAL
		live |= (tab == TAB_DEBUG) && (s_dbgPage != DBG_ACTIONS);
#endif
		s_lastLiveTick = now;
		if (live)
		{
			s_dirty = 1;
		}
	}
}

int N3DS_BottomUi_NeedsPaint(void)
{
	return s_dirty;
}

const unsigned short *N3DS_BottomUi_Paint(int force)
{
	if (!s_dirty && !force)
	{
		return NULL;
	}
	s_dirty = 0;
	ui_draw_all();
	ui_tile();
	return s_tiled;
}

void N3DS_BottomUi_NoteGpuOwned(void)
{
	s_gpuOwned = 1;
}

int N3DS_BottomUi_MapTabActive(void)
{
	return s_tab == TAB_MAP;
}

void N3DS_BottomUi_MapLive(int live)
{
	if (!live)
	{
		if (s_mapZeroFrames < 1000)
		{
			s_mapZeroFrames++;
		}
		if (s_mapLive && (s_mapZeroFrames >= 90))
		{
			s_mapLive = 0;
			s_dirty = 1;
		}
		return;
	}
	if (!s_mapLive)
	{
		s_mapLive = 1;
		s_dirty = 1;
		/* A new race's map: bring MAPA up once per session (a pause or a
		 * one-frame gap does not count: 90 frames without a map end it). */
		if (s_mapZeroFrames >= 90)
		{
			s_tab = TAB_MAP;
		}
	}
	s_mapZeroFrames = 0;
}

void N3DS_BottomUi_LogLine(const char *text)
{
	char *dst;
	unsigned slot;
	size_t n;

	if (text == NULL)
	{
		return;
	}
	if (strncmp(text, "[CTR-3DS] ", 10) == 0)
	{
		text += 10;
	}
	slot = __atomic_fetch_add(&s_logHead, 1U, __ATOMIC_RELAXED) % LOG_LINES;
	dst = s_logRing[slot];
	n = strcspn(text, "\r\n");
	if (n > LOG_COLS)
	{
		n = LOG_COLS;
	}
	memcpy(dst, text, n);
	dst[n] = '\0';
#ifdef CTR_INTERNAL
	if ((s_tab == TAB_DEBUG) && (s_dbgPage == DBG_LOG))
	{
		s_dirty = 1;
	}
#endif
}

void N3DS_BottomUi_SetStatus(const char *text)
{
	snprintf(s_status, sizeof(s_status), "%s", (text != NULL) ? text : "");
	s_dirty = 1;
	if (!s_gpuOwned)
	{
		ui_cpu_present();
	}
}
