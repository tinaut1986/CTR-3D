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
#include <time.h>

#include "n3ds_backend.h"
#include "n3ds_bottomui.h"
#include "n3ds_debuglink.h"
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

/* HUD AUTO-HIDE (OPTIONS): while the MAPA tab is up, the race HUD is drawn below beside the
 * map and left off the top screen. Saved as "hudhide" in ctr3d.cfg. */
int g_dbg_hudAutoHide = 0;
/* SHOW FPS (OPTIONS): the fps readout on the top screen. Saved as "showfps". */
int g_dbg_showFps = 0;
#ifdef CTR_INTERNAL
extern int g_dbg_drawMerge;
extern int g_dbg_renderThread;
#endif

/* ------------------------------------------------------------------ state --- */

enum
{
	TAB_MAP = 0,
	TAB_UPDATER,
	TAB_OPTIONS,
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

static volatile int s_tab = TAB_MAP;
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
static unsigned s_ftpSig = 0;
/* In-game updater page (updater.h). The release notes are word-wrapped once on
 * the game thread when a check finishes (Update_FormatNotes keeps a 12 KB buffer
 * on the stack, too much for the render worker that paints), then only drawn. */
#define NOTE_LINES 384
#define NOTE_ROWS 4
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
/* Themes: the palette of the whole panel, named after the game (CRASH is the
 * default: wumpa orange on crate brown). Every COL_* below reads the current
 * one, so a theme change repaints everything. Saved as "uitheme" in ctr3d.cfg. */
struct UiTheme
{
	const char *name;
	u16 bg, bar, cell, cellEdge, tab, tabEdge, on, onEdge, onBody;
	u16 accent, text, dim, faint, good, warn, bad, btn, btnEdge;
};

static const struct UiTheme s_themes[] = {
	{ "CRASH", RGB(26, 18, 12), RGB(14, 9, 6), RGB(58, 38, 20), RGB(120, 78, 34), RGB(44, 30, 16), RGB(96, 64, 30), RGB(236, 126, 24),
	  RGB(255, 200, 80), RGB(120, 58, 10), RGB(255, 170, 40), RGB(250, 240, 220), RGB(180, 150, 110), RGB(110, 84, 54), RGB(150, 230, 90),
	  RGB(255, 210, 70), RGB(240, 90, 70), RGB(46, 110, 60), RGB(110, 200, 110) },
	{ "NITRO", RGB(10, 20, 12), RGB(6, 12, 8), RGB(18, 48, 26), RGB(46, 140, 60), RGB(14, 36, 20), RGB(36, 100, 48), RGB(110, 240, 70),
	  RGB(200, 255, 120), RGB(30, 100, 30), RGB(130, 255, 80), RGB(230, 255, 225), RGB(130, 180, 130), RGB(60, 100, 64), RGB(130, 255, 90),
	  RGB(255, 225, 60), RGB(255, 90, 110), RGB(60, 40, 110), RGB(160, 110, 240) },
	{ "TNT", RGB(24, 10, 10), RGB(12, 5, 5), RGB(60, 18, 18), RGB(150, 40, 36), RGB(44, 14, 14), RGB(110, 30, 28), RGB(240, 60, 44),
	  RGB(255, 170, 100), RGB(130, 24, 20), RGB(255, 214, 60), RGB(255, 236, 220), RGB(190, 140, 130), RGB(110, 64, 60), RGB(160, 235, 100),
	  RGB(255, 214, 60), RGB(255, 110, 90), RGB(150, 36, 30), RGB(255, 150, 90) },
	{ "OXIDE", RGB(10, 12, 34), RGB(6, 7, 22), RGB(20, 28, 70), RGB(60, 90, 200), RGB(16, 22, 56), RGB(46, 66, 150), RGB(60, 220, 255),
	  RGB(190, 250, 255), RGB(16, 80, 130), RGB(80, 230, 255), RGB(230, 240, 255), RGB(130, 150, 200), RGB(64, 76, 130), RGB(110, 240, 170),
	  RGB(255, 210, 90), RGB(255, 100, 130), RGB(70, 40, 130), RGB(190, 120, 255) },
};
#define UI_THEMES ((int)(sizeof(s_themes) / sizeof(s_themes[0])))
static volatile int s_theme = 0;
static int s_themeLoaded = 0;
#define TH (&s_themes[s_theme])

/* A colour darkened (num/16 of it; > 16 lightens, clamped). */
static u16 ui_shade(u16 c, int num)
{
	int r = (((c >> 11) & 31) * num) / 16, g = (((c >> 6) & 31) * num) / 16, b = (((c >> 1) & 31) * num) / 16;

	r = (r > 31) ? 31 : r;
	g = (g > 31) ? 31 : g;
	b = (b > 31) ? 31 : b;
	return (u16)((r << 11) | (g << 6) | (b << 1) | 1);
}

#define COL_BG (TH->bg)
#define COL_MAPBG ui_shade(TH->bg, 8)
#define COL_TEXT (TH->text)
#define COL_LABEL (TH->dim)
#define COL_GOOD (TH->good)
#define COL_WARN (TH->warn)
#define COL_BAD (TH->bad)
#define COL_BAR (TH->bar)
#define COL_TAB_ON (TH->onBody)
#define COL_TAB_OFF (TH->tab)
#define COL_SEP (TH->cellEdge)
#define COL_ACCENT (TH->accent)
#define COL_BTN_ON (TH->btn)
#define COL_BTN_OFF (TH->cell)
#define COL_BTN_ACT (TH->cell)
#define COL_PRESS (TH->onEdge)
#define COL_EDGE (TH->cellEdge)
#define COL_PANEL (TH->cell)

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
	const u16 shadow = ui_shade(COL_BG, 4);
	int pass;
	const int x0 = x;

	/* Pass 0 is a 1 px drop shadow (the game's own text is outlined), pass 1 the text. */
	for (pass = 0; pass < 2; pass++)
	{
		const char *t = s;
		const int o = (pass == 0) ? scale : 0;

		x = x0;
		for (; *t != '\0'; t++)
		{
			const u8 *g = ui_glyph(*t);
			int row, col;

			for (row = 0; row < 7; row++)
			{
				for (col = 0; col < 5; col++)
				{
					if (g[row] & (0x10 >> col))
					{
						ui_rect(x + (col * scale) + o, y + (row * scale) + o, scale, scale, (pass == 0) ? shadow : c);
					}
				}
			}
			x += 6 * scale;
		}
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

	ids[n++] = TAB_MAP;
	ids[n++] = TAB_UPDATER;
	ids[n++] = TAB_OPTIONS;
#ifdef CTR_INTERNAL
	ids[n++] = TAB_DEBUG;
#endif
	return n;
}

/* Tab buttons: icons only, like the author's other ports (sm-3ds, mzm). Slot i
 * is a 30x21 button at x = 4 + 34 * i; the right part of the bar holds the
 * battery, wifi and clock. There is no ACHIEVEMENTS tab: this port has no
 * achievement system, and a dead tab would only mislead. */
#define TABBTN_X0 4
#define TABBTN_PITCH 34
#define TABBTN_W 30
#define TABBTN_H 21

static void ui_tab_rect(int slot, int *x, int *y, int *w, int *h)
{
	*x = TABBTN_X0 + (slot * TABBTN_PITCH);
	*y = 2;
	*w = TABBTN_W;
	*h = TABBTN_H;
}

/* 14x14 glyphs made of rectangles, centred on (x, y). */
static void ui_tab_icon(int id, int x, int y, u16 c)
{
#define R(dx, dy, w, h) ui_rect(x + (dx), y + (dy), (w), (h), c)
	switch (id)
	{
	case TAB_MAP: /* floor plan: 3x3 rooms, hollow centre */
		R(-7, -7, 4, 4); R(-2, -7, 4, 4); R(3, -7, 4, 4);
		R(-7, -2, 4, 4);                  R(3, -2, 4, 4);
		R(-7, 3, 4, 4);  R(-2, 3, 4, 4);  R(3, 3, 4, 4);
		break;
	case TAB_UPDATER: /* arrow down into a tray */
		R(-1, -7, 3, 7); R(-5, -1, 11, 1); R(-4, 0, 9, 1); R(-3, 1, 7, 1); R(-2, 2, 5, 1);
		R(-6, 4, 13, 1); R(-6, 4, 1, 3); R(6, 4, 1, 3); R(-6, 6, 13, 1);
		break;
	case TAB_OPTIONS: /* sliders */
		R(-6, -4, 12, 1); R(-6, 0, 12, 1); R(-6, 4, 12, 1);
		R(-2, -5, 3, 3); R(2, -1, 3, 3); R(-5, 3, 3, 3);
		break;
#ifdef CTR_INTERNAL
	default: /* bug */
		R(-1, -7, 1, 2); R(1, -7, 1, 2); R(-2, -5, 4, 2);
		R(-3, -3, 3, 8); R(1, -3, 3, 8); R(-2, -4, 4, 1); R(-2, 5, 4, 1);
		R(-6, -2, 3, 1); R(-6, 1, 3, 1); R(-6, 4, 3, 1);
		R(3, -2, 3, 1); R(3, 1, 3, 1); R(3, 4, 3, 1);
		break;
#else
	default: break;
#endif
	}
#undef R
}

/* Battery, wifi and clock at the right of the bar. Polled by Frame (s_sys*). */
static int s_sysBattery = 5, s_sysCharging = 0, s_sysWifi = 0, s_sysPtm = 0;
static char s_sysClock[8] = "--:--";
static int ui_poll_system(void);

static void ui_draw_system(void)
{
	const u16 frame = COL_LABEL, dim = TH->faint;
	const int cy = 13;
	const int bx = 299, by = cy - 4;
	const int wx = bx - 16;
	int i;

	ui_rect(bx, by, 15, 1, frame);
	ui_rect(bx, by + 7, 15, 1, frame);
	ui_rect(bx, by, 1, 8, frame);
	ui_rect(bx + 14, by, 1, 8, frame);
	ui_rect(bx + 15, by + 2, 2, 4, frame);
	if (s_sysPtm)
	{
		const int w = (s_sysBattery * 11) / 5;
		const u16 fill = s_sysCharging ? RGB(90, 210, 120)
		                 : (s_sysBattery <= 1) ? RGB(230, 80, 60)
		                 : (s_sysBattery == 2) ? RGB(235, 190, 70)
		                                       : RGB(110, 205, 130);

		if (w > 0)
		{
			ui_rect(bx + 2, by + 2, w, 4, fill);
		}
		if (s_sysCharging)
		{
			/* plugged in: a lightning bolt over the battery (5x8, bit 4 = left) */
			static const u8 bolt[8] = { 0x03, 0x06, 0x0C, 0x1E, 0x07, 0x06, 0x0C, 0x18 };
			int row, col;

			for (row = 0; row < 8; row++)
			{
				for (col = 0; col < 5; col++)
				{
					if (bolt[row] & (0x10 >> col))
					{
						ui_rect(bx + 5 + col, by + row, 1, 1, RGB(255, 245, 160));
					}
				}
			}
		}
	}
	else
	{
		ui_rect(bx + 5, by + 3, 4, 2, dim);
	}
	for (i = 0; i < 3; i++)
	{
		const int h = 3 + (i * 2);

		ui_rect(wx + (i * 4), cy + 4 - h, 2, h, (i < s_sysWifi) ? frame : dim);
	}
	ui_text(wx - 4 - ui_text_w(1, s_sysClock), cy - 3, 1, COL_TEXT, s_sysClock);
}

enum
{
	OPT_HUDHIDE = 0,
	OPT_STEREO,
	OPT_FPSCAP,
	OPT_SHOWFPS,
	OPT_CPU804,
	OPT_THEME,
	OPT_COUNT
};

static int s_isNew3DS = -1;

static int ui_is_new3ds(void)
{
	if (s_isNew3DS < 0)
	{
		bool n = false;

		APT_CheckNew3DS(&n);
		s_isNew3DS = n ? 1 : 0;
	}
	return s_isNew3DS;
}

/* Two columns of 151x24 cells: the four display switches, then (below the
 * SYSTEM header) CPU 804MHZ (New 3DS only) and the THEME. */
static void ui_opt_rect(int i, int *x, int *y, int *w, int *h)
{
	int slot = 0;

	*w = 151;
	*h = 24;
	if (i <= OPT_SHOWFPS)
	{
		*x = 6 + ((i & 1) * 157);
		*y = 44 + ((i >> 1) * 27);
		return;
	}
	if (i == OPT_CPU804)
	{
		if (!ui_is_new3ds())
		{
			*w = 0; /* no cell on an Old 3DS: nothing to hit */
		}
	}
	else
	{
		slot = ui_is_new3ds() ? 1 : 0;
	}
	*x = 6 + (slot * 157);
	*y = 114;
}

#ifdef CTR_INTERNAL
enum
{
	ACT_DRAWMERGE = 0,
	ACT_RTHREAD,
	ACT_FLUSH,
	ACT_FTP,
	ACT_O3DSSIM,
	ACT_SDLOG,
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
	*y = 212;
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
	case UPD_CHANNEL: *x = 6; *y = 44; *w = 151; *h = 24; break;
	case UPD_AUTO: *x = 163; *y = 44; *w = 151; *h = 24; break;
	case UPD_ACTION: *x = 6; *y = 72; *w = 308; *h = 24; break;
	case UPD_UP: *x = 280; *y = 154; *w = 34; *h = 26; break;
	default: *x = 280; *y = 182; *w = 34; *h = 26; break;
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
	const int x = ui_text(6, y, 1, COL_LABEL, label);

	ui_text(x + 8, y, 1, vcol, value);
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

/* Mean of the newest `n` frame times. The STATUS tab repaints every 250 ms, so
 * showing the single latest frame (s_lastDtUs) read one random frame per
 * repaint: on the New 3DS, where the game thread's call interval is not
 * vblank-locked, that swung 28-80 while the presented rate sat at a steady 30. */
int N3DS_BottomUi_Snapshot(unsigned char *bgr)
{
	int x, y;

	if (!s_frameSeen)
	{
		return 0;
	}
	for (y = 0; y < UI_H; y++)
	{
		for (x = 0; x < UI_W; x++)
		{
			const u16 p = s_pix[(y * UI_W) + x];
			const u32 r = (p >> 11) & 31U, g = (p >> 6) & 31U, b = (p >> 1) & 31U;
			unsigned char *o = bgr + (((size_t)y * UI_W + (size_t)x) * 3U);

			o[0] = (unsigned char)((b << 3) | (b >> 2));
			o[1] = (unsigned char)((g << 3) | (g >> 2));
			o[2] = (unsigned char)((r << 3) | (r >> 2));
		}
	}
	return 1;
}

unsigned N3DS_BottomUi_AvgFrameUs(void)
{
	u32 mn, mx, av;

	ui_ring_stats(&mn, &mx, &av);
	return av;
}

static u16 ui_time_col(u32 us)
{
	return (us <= 34000U) ? COL_GOOD : (us <= 50000U) ? COL_WARN : COL_BAD;
}

/* A crate-style cell: 1 px edge, a lit top and a shaded bottom inside, and a stud in each corner. */
static void ui_cell(int x, int y, int w, int h, u16 body, u16 edge)
{
	ui_rect(x, y, w, h, edge);
	ui_rect(x + 1, y + 1, w - 2, h - 2, body);
	if ((w >= 16) && (h >= 12))
	{
		ui_rect(x + 1, y + 1, w - 2, 1, ui_shade(body, 20));
		ui_rect(x + 1, y + h - 2, w - 2, 1, ui_shade(body, 11));
		ui_rect(x + 2, y + 2, 1, 1, edge);
		ui_rect(x + w - 3, y + 2, 1, 1, edge);
		ui_rect(x + 2, y + h - 3, 1, 1, edge);
		ui_rect(x + w - 3, y + h - 3, 1, 1, edge);
	}
}

static void ui_button(int x, int y, int w, int h, u16 body, int pressed)
{
	ui_cell(x, y, w, h, body, pressed ? COL_PRESS : COL_EDGE);
}

/* A section title in the accent colour with a rule to its right (and, optionally, a note at the far right). */
static void ui_header(int y, const char *title, const char *right)
{
	const int x = ui_text(6, y, 1, COL_ACCENT, title) + 6;
	const int rw = (right != NULL) ? ui_text_w(1, right) + 6 : 0;

	ui_rect(x, y + 3, 314 - rw - x + 1, 1, COL_EDGE);
	if (right != NULL)
	{
		ui_text(314 - ui_text_w(1, right) + 1, y, 1, COL_LABEL, right);
	}
}

/* The wumpa fruit, 9x9, in its own colours whatever the theme. */
static void ui_wumpa(int x, int y)
{
	static const char *const art[9] = { "..GGG....", ".GG.GG...", "..OOOO...", ".OYOOOO..", "OYOOOORO.",
	                                    "OOOOOORO.", "OOOOORRO.", ".OOOORO..", "..OOOO..." };
	int r, c;

	for (r = 0; r < 9; r++)
	{
		for (c = 0; c < 9; c++)
		{
			u16 col;

			switch (art[r][c])
			{
			case 'G': col = RGB(90, 200, 70); break;
			case 'O': col = RGB(240, 130, 20); break;
			case 'Y': col = RGB(255, 210, 90); break;
			case 'R': col = RGB(170, 70, 10); break;
			default: continue;
			}
			ui_rect(x + c, y + r, 1, 1, col);
		}
	}
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

/* The pre-game "label NN%" status ("loading audio 40%"...) as a modal box over
 * whichever tab is up, instead of bare text in the middle of the screen. */
static void ui_draw_status_modal(void)
{
	const int w = 232, h = 56;
	const int x = (UI_W - w) / 2, y = (UI_H - h) / 2;

	if (s_status[0] == '\0')
	{
		return;
	}
	ui_cell(x - 2, y - 2, w + 4, h + 4, ui_shade(COL_BG, 4), COL_EDGE); /* the dimmed halo */
	ui_cell(x, y, w, h, COL_PANEL, COL_ACCENT);
	ui_text_c(UI_W / 2, y + 12, 1, COL_LABEL, TR("PLEASE WAIT", "ESPERA UN MOMENTO"));
	ui_rect(x + 10, y + 24, w - 20, 1, COL_SEP);
	ui_text_c(UI_W / 2, y + 34, 1, COL_WARN, s_status);
}

static void ui_draw_map(void)
{
	ui_rect(0, CONTENT_Y, UI_W, UI_H - CONTENT_Y, COL_MAPBG);
	if (!g_dbg_btmMap || !s_mapLive)
	{
		ui_cell(60, 86, 200, 68, COL_PANEL, COL_EDGE);
		ui_wumpa(UI_W / 2 - 4, 94);
		ui_text_c(UI_W / 2, 110, 1, COL_TEXT, g_dbg_btmMap ? TR("NO MAP NOW", "SIN MAPA AHORA") : TR("MINIMAP ON THE TOP SCREEN", "MAPA EN LA PANTALLA SUPERIOR"));
		ui_text_c(UI_W / 2, 124, 1, COL_LABEL,
		          g_dbg_btmMap ? TR("IT SHOWS HERE DURING A RACE", "SE VE AQUI DURANTE UNA CARRERA") : TR("SWITCHED OFF IN DEBUG", "DESACTIVADO EN DEBUG"));
	}
}

static int s_optSel = -1; /* the last cell touched: its help shows under the cells */

/* One settings cell: the name dimmed above, the value below (green for on, faint for off). */
static void ui_draw_option(int i, const char *label, const char *stateText, u16 vcol)
{
	int x, y, w, h;

	ui_opt_rect(i, &x, &y, &w, &h);
	ui_cell(x, y, w, h, COL_PANEL, (i == s_pressBtn) ? COL_PRESS : (i == s_optSel) ? COL_ACCENT : COL_EDGE);
	ui_text(x + 6, y + 4, 1, COL_LABEL, label);
	ui_text(x + 6, y + 13, 1, vcol, stateText);
}

static const char *ui_stereo_text(void)
{
	if (g_dbg_stereo == 2)
	{
		return N3DS_FramePaceStereoSuppressed() ? TR("AUTO (2D)", "AUTO (2D)") : "AUTO";
	}
	return ui_onoff(g_dbg_stereo);
}

/* FRAME SKIP: OFF (60 whenever a frame fits), AUTO (60 / 30 / 20, adaptive;
 * shows the rate in force) or 30 (retail). */
static const char *ui_fpscap_text(void)
{
	if (g_dbg_frameCap == 1)
	{
		return "30";
	}
	if (g_dbg_frameCap == 2)
	{
		return "OFF";
	}
	switch (N3DS_FramePaceFlipVBlanks())
	{
	case 1:
		return "AUTO (60)";
	case 3:
		return "AUTO (20)";
	default:
		return "AUTO (30)";
	}
}

static void ui_draw_options(void)
{
	static const char *const help[OPT_COUNT][2] = {
		{ "WITH THE MAP TAB UP THE RACE HUD MOVES HERE", "AND LEAVES THE TOP SCREEN CLEAR." },
		{ "USES THE CONSOLE 3D SLIDER. AUTO TURNS IT", "OFF WHEN THE GAME RUNS SLOW." },
		{ "AUTO PICKS THE FRAME RATE THE CONSOLE CAN", "HOLD; 30 LOCKS IT; OFF DRAWS EVERY FRAME." },
		{ "FPS COUNTER ON THE TOP SCREEN, TOP CENTRE.", "" },
		{ "NEW 3DS ONLY. OFF SHOWS HOW THE GAME RUNS", "ON AN OLD 3DS." },
		{ "COLOURS OF THIS SCREEN: CRASH, NITRO, TNT", "AND OXIDE. TAP TO CHANGE." },
	};
	static const char *const helpEs[OPT_COUNT][2] = {
		{ "CON LA PESTANA MAPA, EL HUD DE CARRERA PASA", "AQUI Y LA PANTALLA SUPERIOR QUEDA LIBRE." },
		{ "USA EL CONTROL 3D DE LA CONSOLA. AUTO LO", "APAGA SI EL JUEGO VA LENTO." },
		{ "AUTO ELIGE EL RITMO QUE AGUANTA LA CONSOLA;", "30 LO FIJA; OFF DIBUJA TODOS LOS FRAMES." },
		{ "CONTADOR DE FPS ARRIBA, EN EL CENTRO.", "" },
		{ "SOLO NEW 3DS. APAGADO MUESTRA COMO VA EL", "JUEGO EN UNA OLD 3DS." },
		{ "COLORES DE ESTA PANTALLA: CRASH, NITRO, TNT", "Y OXIDE. TOCA PARA CAMBIAR." },
	};
	char c[32];
	const int newConsole = ui_is_new3ds();

	ui_header(32, TR("DISPLAY", "PANTALLA"), NULL);
	ui_draw_option(OPT_HUDHIDE, TR("AUTO-HIDE HUD", "HUD AUTOOCULTO"), ui_onoff(g_dbg_hudAutoHide), g_dbg_hudAutoHide ? COL_GOOD : TH->faint);
	ui_draw_option(OPT_STEREO, TR("3D EFFECT", "EFECTO 3D"), ui_stereo_text(), g_dbg_stereo ? COL_TEXT : TH->faint);
	ui_draw_option(OPT_FPSCAP, TR("FRAME SKIP", "SALTO DE FRAMES"), ui_fpscap_text(), COL_TEXT);
	ui_draw_option(OPT_SHOWFPS, TR("SHOW FPS", "VER FPS"), ui_onoff(g_dbg_showFps), g_dbg_showFps ? COL_GOOD : TH->faint);
	ui_header(102, TR("SYSTEM", "SISTEMA"), NULL);
	/* CPU 804 MHZ is a New 3DS switch; an Old 3DS has no clock-up, so it has no cell. */
	if (newConsole)
	{
		ui_draw_option(OPT_CPU804, "CPU 804 MHZ", ui_onoff(N3DS_SpeedupEnabled()), N3DS_SpeedupEnabled() ? COL_GOOD : TH->faint);
	}
	snprintf(c, sizeof(c), "< %s >", TH->name);
	ui_draw_option(OPT_THEME, TR("THEME", "TEMA"), c, COL_ACCENT);
	ui_header(145, TR("HELP", "AYUDA"), NULL);
	if ((s_optSel >= 0) && (s_optSel < OPT_COUNT))
	{
		ui_text(6, 160, 1, COL_LABEL, s_es ? helpEs[s_optSel][0] : help[s_optSel][0]);
		ui_text(6, 171, 1, COL_LABEL, s_es ? helpEs[s_optSel][1] : help[s_optSel][1]);
	}
	else
	{
		ui_text(6, 160, 1, COL_LABEL, TR("TAP A SETTING TO CHANGE IT.", "TOCA UN AJUSTE PARA CAMBIARLO."));
	}
	ui_wumpa(6, 223);
	if (s_msg[0] != '\0')
	{
		ui_text(20, 224, 1, COL_GOOD, s_msg);
	}
	else
	{
		ui_text(20, 224, 1, TH->faint, "CTR 3D " CTR_NATIVE_VERSION);
	}
}

/* A progress bar in hazard stripes (the TNT crates' paint): lit up to the progress, dim beyond it. */
static void ui_draw_bar(int x, int y, int w, int h, unsigned long long done, unsigned long long total, u16 fill)
{
	const int fw = (total > 0) ? (int)(((unsigned long long)w * (done > total ? total : done)) / total) : 0;
	const u16 dark = ui_shade(COL_BG, 6);
	int i, j;

	for (i = 0; i < w; i++)
	{
		for (j = 0; j < h; j++)
		{
			const int stripe = (((i + j) / 4) & 1);
			const u16 lit = stripe ? fill : dark;

			ui_rect(x + i, y + j, 1, 1, (i < fw) ? lit : ui_shade(lit, 5));
		}
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
	ui_header(32, TR("UPDATER", "ACTUALIZADOR"), CTR_NATIVE_VERSION);

	/* Channel and auto-check, as settings cells. */
	ui_upd_rect(UPD_CHANNEL, &x, &y, &w, &h);
	ui_cell(x, y, w, h, COL_PANEL, (s_pressBtn == UPD_BTN_BASE + UPD_CHANNEL) ? COL_PRESS : COL_EDGE);
	ui_text(x + 6, y + 4, 1, COL_LABEL, TR("CHANNEL", "CANAL"));
	ui_text(x + 6, y + 13, 1, busy ? TH->faint : COL_ACCENT, st.betas ? TR("+ BETAS", "+ BETAS") : TR("STABLE", "ESTABLE"));
	ui_upd_rect(UPD_AUTO, &x, &y, &w, &h);
	{
		const int on = Updater_AutoCheckEnabled();

		ui_cell(x, y, w, h, COL_PANEL, (s_pressBtn == UPD_BTN_BASE + UPD_AUTO) ? COL_PRESS : COL_EDGE);
		ui_text(x + 6, y + 4, 1, COL_LABEL, TR("CHECK AT START", "BUSCAR AL ABRIR"));
		ui_text(x + 6, y + 13, 1, on ? COL_GOOD : TH->faint, ui_onoff(on));
	}

	/* The action button follows the state. */
	{
		const char *label = TR("CHECK FOR UPDATES", "BUSCAR ACTUALIZACION");
		u16 col = COL_BTN_ACT;
		int lit = 0;

		if (st.state == UPDATE_CHECKING)
		{
			label = TR("CHECKING...", "BUSCANDO...");
			col = ui_shade(COL_PANEL, 12);
		}
		else if (st.state == UPDATE_AVAILABLE)
		{
			label = TR("DOWNLOAD AND INSTALL", "DESCARGAR E INSTALAR");
			col = COL_BTN_ON;
			lit = 1;
		}
		else if (st.state == UPDATE_DOWNLOADING)
		{
			label = TR("CANCEL DOWNLOAD", "CANCELAR DESCARGA");
			col = COL_BAD;
			col = ui_shade(col, 8);
		}
		else if ((st.state == UPDATE_VERIFYING) || (st.state == UPDATE_INSTALLING))
		{
			label = TR("INSTALLING...", "INSTALANDO...");
			col = ui_shade(COL_PANEL, 12);
		}
		else if (st.state == UPDATE_DONE)
		{
			label = TR("RESTARTING...", "REINICIANDO...");
			col = ui_shade(COL_PANEL, 12);
		}
		ui_upd_rect(UPD_ACTION, &x, &y, &w, &h);
		ui_cell(x, y, w, h, col, (s_pressBtn == UPD_BTN_BASE + UPD_ACTION) ? COL_PRESS : lit ? TH->btnEdge : COL_EDGE);
		ui_text_c(x + (w / 2), y + 8, 1, COL_TEXT, label);
		if (lit)
		{
			ui_wumpa(x + 8, y + 7);
			ui_wumpa(x + w - 17, y + 7);
		}
	}

	/* Status line, found version and progress. */
	y = 103;
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
		ui_draw_bar(6, 115, 308, 8, st.progress, 100, COL_WARN);
		snprintf(c, sizeof(c), "%u%%", (unsigned)st.progress);
		ui_text(6, 126, 1, COL_LABEL, c);
	}
	else if (Updater_CannotInstall())
	{
		ui_text(6, 116, 1, COL_WARN, TR("RUN THE INSTALLED CIA TO UPDATE", "USA EL CIA INSTALADO PARA ACTUALIZAR"));
	}

	/* Release notes (the newer releases of the channel, or the whole changelog when up to date). */
	ui_header(142, TR("WHAT'S NEW", "NOVEDADES"), NULL);
	ui_cell(6, 154, 270, 54, ui_shade(COL_BG, 14), COL_EDGE);
	if (s_noteCount > 0)
	{
		for (i = 0; i < NOTE_ROWS; i++)
		{
			const unsigned line = s_noteScroll + (unsigned)i;

			if (line < s_noteCount)
			{
				ui_text(10, 159 + (i * 11), 1, COL_LABEL, s_noteLines[line]);
			}
		}
		if (s_noteCount > NOTE_ROWS)
		{
			ui_upd_rect(UPD_UP, &x, &y, &w, &h);
			ui_button(x, y, w, h, COL_TAB_OFF, (s_pressBtn == UPD_BTN_BASE + UPD_UP));
			ui_text_c(x + (w / 2), y + 9, 1, COL_TEXT, "+");
			ui_upd_rect(UPD_DOWN, &x, &y, &w, &h);
			ui_button(x, y, w, h, COL_TAB_OFF, (s_pressBtn == UPD_BTN_BASE + UPD_DOWN));
			ui_text_c(x + (w / 2), y + 9, 1, COL_TEXT, "-");
		}
	}
	else
	{
		ui_text(10, 159, 1, TH->faint, TR("NOTHING TO SHOW YET.", "NADA QUE MOSTRAR TODAVIA."));
	}

	/* The FTP side (developers): address while it listens, CANCEL in install-only mode. */
	y = 212;
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
	ui_text(6, y, 1, ui_time_col(av), c);
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
	case ACT_SDLOG: *name = TR("LOG TO SD", "LOG A LA SD"); on = g_dbg_sdLog; break;
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
		ui_text(x + 6, y + 22, 1, COL_TEXT, state);
	}
	ui_text(6, CONTENT_Y + 174, 1, COL_LABEL, TR("SAME SWITCHES AS SITE DUMPREQ", "LOS MISMOS INTERRUPTORES QUE SITE DUMPREQ"));
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
	const int tab = s_tab;
	int i;

	if (!s_themeLoaded)
	{
		s_themeLoaded = 1;
		s_theme = N3DS_ConfigGet("uitheme", 0);
		if ((s_theme < 0) || (s_theme >= UI_THEMES))
		{
			s_theme = 0;
		}
	}
	ui_rect(0, 0, UI_W, UI_H, COL_BG);
	/* Tab bar: icon buttons on the left (the selected one lit), battery, wifi
	 * and clock on the right, and a rule under the bar. */
	ui_rect(0, 0, UI_W, TAB_H, COL_BAR);
	for (i = 0; i < n; i++)
	{
		const int on = (ids[i] == tab);
		int x, y, w, h;

		ui_tab_rect(i, &x, &y, &w, &h);
		ui_rect(x, y, w, h, on ? TH->onEdge : TH->tabEdge);
		ui_rect(x + 1, y + 1, w - 2, h - 2, on ? TH->onBody : TH->tab);
		if (on)
		{
			ui_rect(x + 1, y + 1, w - 2, 1, TH->on);
		}
		ui_tab_icon(ids[i], x + (w / 2), y + (h / 2), on ? COL_TEXT : COL_LABEL);
		if ((ids[i] == TAB_UPDATER) && s_updNew)
		{
			ui_rect(x + w - 7, y + 2, 5, 5, COL_WARN); /* a newer build was found */
		}
	}
	ui_draw_system();
	ui_rect(0, TAB_H - 2, UI_W, 2, COL_SEP);

	switch (tab)
	{
	case TAB_MAP: ui_draw_map(); break;
	case TAB_OPTIONS: ui_draw_options(); break;
	case TAB_UPDATER: ui_draw_updater(); break;
#ifdef CTR_INTERNAL
	case TAB_DEBUG: ui_draw_debug(); break;
#endif
	default: break;
	}
	ui_draw_banner();
	ui_draw_status_modal();
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
	s_optSel = i;
	switch (i)
	{
	case OPT_HUDHIDE:
		g_dbg_hudAutoHide ^= 1;
		N3DS_ConfigSet("hudhide", g_dbg_hudAutoHide);
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
		/* AUTO -> 30 -> OFF -> AUTO. */
		g_dbg_frameCap = (g_dbg_frameCap + 1) % 3;
		N3DS_ConfigSet("fpsmode", g_dbg_frameCap);
		break;
	case OPT_SHOWFPS:
		g_dbg_showFps ^= 1;
		N3DS_ConfigSet("showfps", g_dbg_showFps);
		break;
	case OPT_CPU804:
		if (s_isNew3DS > 0)
		{
			N3DS_SetSpeedup(!N3DS_SpeedupEnabled());
			N3DS_ConfigSet("cpu804", N3DS_SpeedupEnabled());
			ui_set_msg(N3DS_SpeedupEnabled() ? TR("CPU 804MHZ", "CPU 804MHZ") : TR("OLD 3DS SPEED: SLOWER", "VELOCIDAD OLD 3DS: MAS LENTO"));
		}
		break;
	case OPT_THEME:
		s_theme = (s_theme + 1) % UI_THEMES;
		N3DS_ConfigSet("uitheme", s_theme);
		break;
	default:
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
	case ACT_SDLOG:
		N3DS_SetSdLogEnabled(!g_dbg_sdLog);
		ui_set_msg(g_dbg_sdLog ? TR("SD LOG ON (CAN CAUSE HITCHES)", "LOG EN SD ACTIVO (PUEDE DAR TIRONES)") : TR("SD LOG OFF", "LOG EN SD APAGADO"));
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
#ifdef CTR_INTERNAL
	int tx = 0, ty = 0;
	const int injected = N3DS_DebugLinkTouch(&tx, &ty);
#else
	const int injected = 0;
#endif

	if (!injected && !(hidKeysDown() & KEY_TOUCH))
	{
		return;
	}
	if (injected)
	{
#ifdef CTR_INTERNAL
		t.px = (u16)tx;
		t.py = (u16)ty;
#endif
	}
	else
	{
		hidTouchRead(&t);
	}

	if (t.py < TAB_H)
	{
		n = ui_tab_list(ids);
		i = (t.px - TABBTN_X0) / TABBTN_PITCH;
		if ((t.px >= TABBTN_X0) && (i < n) && (((t.px - TABBTN_X0) % TABBTN_PITCH) < TABBTN_W))
		{
			s_tab = ids[i];
			s_msg[0] = '\0';
			s_dirty = 1;
		}
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
	s_sysPtm = R_SUCCEEDED(ptmuInit());
	ui_poll_system();

	s_dirty = 1;
	/* Both buffers get the first picture, so the first swap shows it whichever
	 * one the display is on. */
	ui_cpu_present();
	s_dirty = 1;
	ui_cpu_present();
}

/* Battery, wifi and clock; true when what the bar shows changed. The console's
 * clock is local time. */
static int ui_poll_system(void)
{
	int bat = s_sysBattery, chg = s_sysCharging;
	const int wifi = (int)osGetWifiStrength();
	char clk[8] = "--:--";
	time_t tt = time(NULL);
	const struct tm *tm = gmtime(&tt);
	int changed;

	if (s_sysPtm)
	{
		u8 lv = 0, c = 0;

		if (R_SUCCEEDED(PTMU_GetBatteryLevel(&lv)))
		{
			bat = lv;
		}
		if (R_SUCCEEDED(PTMU_GetBatteryChargeState(&c)))
		{
			chg = c;
		}
	}
	if (tm != NULL)
	{
		snprintf(clk, sizeof(clk), "%02d:%02d", tm->tm_hour, tm->tm_min);
	}
	changed = (bat != s_sysBattery) || (chg != s_sysCharging) || (wifi != s_sysWifi) || (strcmp(clk, s_sysClock) != 0);
	s_sysBattery = bat;
	s_sysCharging = chg;
	s_sysWifi = wifi;
	memcpy(s_sysClock, clk, sizeof(clk));
	return changed;
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
	/* 250 ms; 1 s on the Old-3DS class, where one live repaint (full software
	 * paint, Morton tile, texture copy + flush) is ~10-20 ms of the game
	 * thread: [ENDPROF] btm=2.4 ms a frame in a static menu, nearly all of it
	 * this tick on the STATUS tab. */
	const u64 liveTicks = N3DS_FramePaceOldClass() ? (u64)SYSCLOCK_ARM11 : ((u64)SYSCLOCK_ARM11 / 4ULL);
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
#ifdef CTR_INTERNAL
	N3DS_DebugLinkPoll();
#endif

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
	{
		static u64 s_lastSysTick = 0;

		if ((now - s_lastSysTick) >= (u64)SYSCLOCK_ARM11)
		{
			s_lastSysTick = now;
			if (ui_poll_system())
			{
				s_dirty = 1;
			}
		}
	}
	if ((now - s_lastLiveTick) >= liveTicks)
	{
		int live = 0;

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

int N3DS_BottomUi_GpuOwned(void)
{
	return s_gpuOwned;
}

int N3DS_BottomUi_MapTabActive(void)
{
	return s_tab == TAB_MAP;
}

int N3DS_BottomUi_HudBelow(void)
{
	return g_dbg_hudAutoHide && s_mapLive && (s_tab == TAB_MAP);
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
