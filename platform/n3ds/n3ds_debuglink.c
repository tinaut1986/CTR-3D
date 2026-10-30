#include "n3ds_debuglink.h"

#ifdef CTR_INTERNAL

#include <3ds.h>

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "n3ds_backend.h"
#include "n3ds_bottomui.h"

int Platform_GetVBlankCount(void);

#define LINK_DIR        "sdmc:/3ds/ctr_native/debug"
#define LINK_WAIT_NS    5000000000LL
#define LINK_WAIT_SLICE 50000000LL
#define LINK_RESULT_MAX 640
#define LINK_PEEK_MAX   256

enum LinkOp
{
	LINK_OP_NONE = 0,
	LINK_OP_SHOT,
	LINK_OP_STATE,
	LINK_OP_MARK,
	LINK_OP_SHOTR,
	LINK_OP_POKE
};

/* One request at a time: the FTP server serves one client and one command. */
static LightLock s_lock = 1; /* LightLock_Init's value, so no init call is needed */
static LightEvent s_done;
static int s_doneReady;
static volatile int s_op;
static char s_arg[80];
static char s_result[LINK_RESULT_MAX];
static unsigned char s_pokeBytes[LINK_PEEK_MAX];
static unsigned s_pokeLen;
static unsigned s_pokeAddr;

/* Injected input, written by the FTP thread, read by the main thread. */
static volatile unsigned s_keysMask;
static volatile unsigned s_keysFrames;
static volatile int s_touchPending;
static volatile int s_touchX, s_touchY;

static unsigned s_frames; /* presented frames seen by the poll */

static const struct
{
	const char *name;
	unsigned mask;
} s_keyNames[] = {
    {"A", KEY_A},         {"B", KEY_B},           {"X", KEY_X},      {"Y", KEY_Y},
    {"L", KEY_L},         {"R", KEY_R},           {"ZL", KEY_ZL},    {"ZR", KEY_ZR},
    {"START", KEY_START}, {"SELECT", KEY_SELECT}, {"UP", KEY_DUP},   {"DOWN", KEY_DDOWN},
    {"LEFT", KEY_DLEFT},  {"RIGHT", KEY_DRIGHT},
};

static int ParseKeys(const char *text, unsigned *mask)
{
	char word[16];

	*mask = 0;
	while (*text && (*text != ' '))
	{
		unsigned n = 0, i;
		int found = 0;

		while (*text && (*text != '+') && (*text != ' ') && ((n + 1) < sizeof(word)))
		{
			word[n++] = (char)toupper((unsigned char)*text++);
		}
		word[n] = 0;
		if (*text == '+')
		{
			text++;
		}
		if (strcmp(word, "NONE") == 0)
		{
			continue;
		}
		for (i = 0; i < (sizeof(s_keyNames) / sizeof(s_keyNames[0])); i++)
		{
			if (strcmp(word, s_keyNames[i].name) == 0)
			{
				*mask |= s_keyNames[i].mask;
				found = 1;
			}
		}
		if (!found)
		{
			return 0;
		}
	}
	return 1;
}

/* Readable (or writable) for `length` bytes, page by page: a bad address from the
 * PC must not crash the game. */
static int MemoryOk(unsigned address, unsigned length, int write)
{
	unsigned at = address;
	const unsigned end = address + length;

	if (end < address)
	{
		return 0;
	}
	while (at < end)
	{
		MemInfo info;
		PageInfo page;

		if (R_FAILED(svcQueryMemory(&info, &page, (u32)at)))
		{
			return 0;
		}
		if (info.state == MEMSTATE_FREE)
		{
			return 0;
		}
		if (!(info.perm & (write ? MEMPERM_WRITE : MEMPERM_READ)))
		{
			return 0;
		}
		at = info.base_addr + info.size;
	}
	return 1;
}

static int ParseHex(const char *text, unsigned char *out, unsigned cap)
{
	unsigned n = 0;

	while (text[0] && text[1] && (n < cap))
	{
		char pair[3] = {text[0], text[1], 0};
		char *end;
		const long v = strtol(pair, &end, 16);

		if (*end)
		{
			return -1;
		}
		out[n++] = (unsigned char)v;
		text += 2;
	}
	return text[0] ? -1 : (int)n;
}

/* Hands the request to the main thread and waits for its result line. */
static void RunOnMainThread(int op, const char *arg, char *reply, unsigned cap)
{
	long long waited;
	int done = 0;

	LightLock_Lock(&s_lock);
	if (!s_doneReady)
	{
		LightEvent_Init(&s_done, RESET_ONESHOT);
		s_doneReady = 1;
	}
	LightEvent_Clear(&s_done);
	s_result[0] = 0;
	snprintf(s_arg, sizeof(s_arg), "%s", arg ? arg : "");
	__atomic_store_n(&s_op, op, __ATOMIC_RELEASE);
	for (waited = 0; waited < LINK_WAIT_NS; waited += LINK_WAIT_SLICE)
	{
		if (LightEvent_WaitTimeout(&s_done, LINK_WAIT_SLICE) == 0)
		{
			done = 1;
			break;
		}
	}
	if (!done)
	{
		__atomic_store_n(&s_op, LINK_OP_NONE, __ATOMIC_RELEASE);
		snprintf(reply, cap, "451 The game did not take the request (loading, or stopped).");
	}
	else
	{
		snprintf(reply, cap, "%s", s_result);
	}
	LightLock_Unlock(&s_lock);
}

int N3DS_DebugLinkCommand(const char *verb, const char *args, char *reply, unsigned cap)
{
	if (args == NULL)
	{
		args = "";
	}
	if (strcmp(verb, "SHOT") == 0)
	{
		RunOnMainThread(LINK_OP_SHOT, "", reply, cap);
	}
	else if (strcmp(verb, "STATE") == 0)
	{
		RunOnMainThread(LINK_OP_STATE, "", reply, cap);
	}
	else if (strcmp(verb, "MARK") == 0)
	{
		RunOnMainThread(LINK_OP_MARK, args, reply, cap);
	}
	else if (strcmp(verb, "SHOTR") == 0)
	{
		RunOnMainThread(LINK_OP_SHOTR, "", reply, cap);
	}
	else if (strcmp(verb, "PEEK") == 0)
	{
		unsigned addr = 0, len = 0, i;
		char *end;
		const char *sp = strchr(args, ' ');

		addr = (unsigned)strtoul(args, &end, 16);
		len = (sp != NULL) ? (unsigned)strtoul(sp + 1, NULL, 0) : 0u;
		if ((sp == NULL) || (len == 0) || (len > LINK_PEEK_MAX))
		{
			snprintf(reply, cap, "501 SITE PEEK <hex address> <length 1..%d>", LINK_PEEK_MAX);
		}
		else if (!MemoryOk(addr, len, 0))
		{
			snprintf(reply, cap, "550 Not readable.");
		}
		else
		{
			const unsigned char *m = (const unsigned char *)(uintptr_t)addr;
			unsigned n = (unsigned)snprintf(reply, cap, "200 ");

			for (i = 0; (i < len) && ((n + 3) < cap); i++)
			{
				n += (unsigned)snprintf(reply + n, cap - n, "%02x", m[i]);
			}
		}
	}
	else if (strcmp(verb, "POKE") == 0)
	{
		char *end;
		const char *sp = strchr(args, ' ');
		const unsigned addr = (unsigned)strtoul(args, &end, 16);
		int n;

		if (sp == NULL)
		{
			snprintf(reply, cap, "501 SITE POKE <hex address> <hex bytes>");
			return 1;
		}
		n = ParseHex(sp + 1, s_pokeBytes, LINK_PEEK_MAX);
		if (n <= 0)
		{
			snprintf(reply, cap, "501 SITE POKE <hex address> <hex bytes>");
			return 1;
		}
		if (!MemoryOk(addr, (unsigned)n, 1))
		{
			snprintf(reply, cap, "550 Not writable.");
			return 1;
		}
		s_pokeAddr = addr;
		s_pokeLen = (unsigned)n;
		RunOnMainThread(LINK_OP_POKE, "", reply, cap);
	}
	else if (strcmp(verb, "DUMPREQ") == 0)
	{
		if (*args == 0)
		{
			snprintf(reply, cap, "501 SITE DUMPREQ <word>");
		}
		else
		{
			char word[48];

			snprintf(word, sizeof(word), "%s", args);
			N3DS_QueueDumpReq(word);
			snprintf(reply, cap, "200 dumpreq %s queued", word);
		}
	}
	else if (strcmp(verb, "KEYS") == 0)
	{
		unsigned mask;
		unsigned frames = 1;
		const char *sp = strchr(args, ' ');

		if ((*args == 0) || !ParseKeys(args, &mask))
		{
			snprintf(reply, cap, "501 SITE KEYS <A+B+LEFT|NONE> [frames]");
			return 1;
		}
		if (sp != NULL)
		{
			frames = (unsigned)strtoul(sp + 1, NULL, 10);
			if (frames == 0)
			{
				frames = 1;
			}
			if (frames > 3600)
			{
				frames = 3600;
			}
		}
		s_keysMask = mask;
		s_keysFrames = (mask != 0) ? frames : 0;
		snprintf(reply, cap, "200 keys %08x for %u frame(s)", mask, (mask != 0) ? frames : 0u);
	}
	else if (strcmp(verb, "TOUCH") == 0)
	{
		int x, y;

		if (sscanf(args, "%d %d", &x, &y) != 2)
		{
			snprintf(reply, cap, "501 SITE TOUCH <x> <y>  (bottom screen, 0..319 / 0..239)");
		}
		else
		{
			s_touchX = x;
			s_touchY = y;
			s_touchPending = 1;
			snprintf(reply, cap, "200 touch %d,%d", x, y);
		}
	}
	else
	{
		return 0;
	}
	return 1;
}

unsigned N3DS_DebugLinkKeys(void)
{
	if (s_keysFrames == 0)
	{
		return 0;
	}
	s_keysFrames--;
	return s_keysMask;
}

int N3DS_DebugLinkTouch(int *x, int *y)
{
	if (!s_touchPending)
	{
		return 0;
	}
	*x = s_touchX;
	*y = s_touchY;
	s_touchPending = 0;
	return 1;
}

/* 24-bit BMP, rows bottom-up; `row(y, dst)` fills one row of width*3 BGR bytes. */
static int WriteBmp(const char *path, int w, int h, void (*row)(int y, unsigned char *dst, void *ctx), void *ctx)
{
	FILE *f = fopen(path, "wb");
	unsigned char hdr[54];
	unsigned char *line;
	const unsigned size = 54u + (unsigned)(w * 3 * h);
	int y;

	if (f == NULL)
	{
		return 0;
	}
	line = (unsigned char *)malloc((size_t)w * 3u);
	if (line == NULL)
	{
		fclose(f);
		return 0;
	}
	memset(hdr, 0, sizeof(hdr));
	hdr[0] = 'B';
	hdr[1] = 'M';
	hdr[2] = (unsigned char)size;
	hdr[3] = (unsigned char)(size >> 8);
	hdr[4] = (unsigned char)(size >> 16);
	hdr[5] = (unsigned char)(size >> 24);
	hdr[10] = 54;
	hdr[14] = 40;
	hdr[18] = (unsigned char)w;
	hdr[19] = (unsigned char)(w >> 8);
	hdr[22] = (unsigned char)h;
	hdr[23] = (unsigned char)(h >> 8);
	hdr[26] = 1;
	hdr[28] = 24;
	fwrite(hdr, 1, sizeof(hdr), f);
	for (y = h - 1; y >= 0; y--)
	{
		row(y, line, ctx);
		fwrite(line, 1, (size_t)w * 3u, f);
	}
	free(line);
	return fclose(f) == 0;
}

static void TopRow(int y, unsigned char *dst, void *ctx)
{
	const unsigned char *fb = (const unsigned char *)ctx;
	int x;

	/* BGR8, column-major, the panel's 240 rows running up. */
	for (x = 0; x < 400; x++)
	{
		const unsigned char *p = fb + (((x * 240) + (239 - y)) * 3);

		dst[(x * 3) + 0] = p[0];
		dst[(x * 3) + 1] = p[1];
		dst[(x * 3) + 2] = p[2];
	}
}

static void BottomRow(int y, unsigned char *dst, void *ctx)
{
	/* ctx = the whole 320x240 picture as BGR, row-major. */
	memcpy(dst, (const unsigned char *)ctx + ((size_t)y * 320u * 3u), 320u * 3u);
}

static void BottomFbRow(int y, unsigned char *dst, void *ctx)
{
	const unsigned char *fb = (const unsigned char *)ctx;
	int x;

	/* Same layout as the top: BGR8, column-major, 240 rows running up; 320 columns. */
	for (x = 0; x < 320; x++)
	{
		const unsigned char *p = fb + (((x * 240) + (239 - y)) * 3);

		dst[(x * 3) + 0] = p[0];
		dst[(x * 3) + 1] = p[1];
		dst[(x * 3) + 2] = p[2];
	}
}

static void DoShot(void)
{
	u16 fw = 0, fh = 0;
	u8 *fb = gfxGetFramebuffer(GFX_TOP, GFX_LEFT, &fw, &fh);
	int topOk = 0, botOk = 0;
	unsigned char *bottom;

	mkdir("sdmc:/3ds", 0777);
	mkdir("sdmc:/3ds/ctr_native", 0777);
	mkdir(LINK_DIR, 0777);
	if (fb != NULL)
	{
		/* Written by the GPU's display transfer, read here by the CPU. */
		GSPGPU_InvalidateDataCache(fb, 400u * 240u * 3u);
		topOk = WriteBmp(LINK_DIR "/top.bmp", 400, 240, TopRow, fb);
	}
	if (N3DS_BottomUi_GpuOwned())
	{
		/* The GPU draws this screen (tabs, map and HUD included): the CPU snapshot below only
		 * knows the CPU-drawn layer, so read the real framebuffer instead. */
		u16 bw = 0, bh = 0;
		u8 *bfb = gfxGetFramebuffer(GFX_BOTTOM, GFX_LEFT, &bw, &bh);

		if (bfb != NULL)
		{
			GSPGPU_InvalidateDataCache(bfb, 320u * 240u * 3u);
			botOk = WriteBmp(LINK_DIR "/bottom.bmp", 320, 240, BottomFbRow, bfb);
		}
	}
	bottom = botOk ? NULL : (unsigned char *)malloc(320u * 240u * 3u);
	if (bottom != NULL)
	{
		if (N3DS_BottomUi_Snapshot(bottom))
		{
			botOk = WriteBmp(LINK_DIR "/bottom.bmp", 320, 240, BottomRow, bottom);
		}
		free(bottom);
	}
	snprintf(s_result, sizeof(s_result), "200 top=%s bottom=%s dir=%s", topOk ? "ok" : "fail", botOk ? "ok" : "fail", "3ds/ctr_native/debug");
}

/* The right eye of the top screen; only meaningful with the 3D slider up. */
static void DoShotRight(void)
{
	u16 fw = 0, fh = 0;
	u8 *fb;
	int ok = 0;

	if (!gfxIs3D())
	{
		snprintf(s_result, sizeof(s_result), "200 3D is off (slider down): no right eye");
		return;
	}
	fb = gfxGetFramebuffer(GFX_TOP, GFX_RIGHT, &fw, &fh);
	mkdir("sdmc:/3ds", 0777);
	mkdir("sdmc:/3ds/ctr_native", 0777);
	mkdir(LINK_DIR, 0777);
	if (fb != NULL)
	{
		GSPGPU_InvalidateDataCache(fb, 400u * 240u * 3u);
		ok = WriteBmp(LINK_DIR "/top-right.bmp", 400, 240, TopRow, fb);
	}
	snprintf(s_result, sizeof(s_result), "200 top-right=%s", ok ? "ok" : "fail");
}

static void DoState(void)
{
	int level = 0, main = 0, load = 0;
	unsigned mode = 0, avgUs = 0;

	N3DS_DebugGameState(&level, &mode, &main, &load);
	avgUs = N3DS_BottomUi_AvgFrameUs();
	snprintf(s_result, sizeof(s_result), "200 frames=%u vbl=%d avgUs=%u fps10=%u lvl=%d mode=%08x main=%d load=%d linear=%luK", s_frames,
	         Platform_GetVBlankCount(), avgUs, (avgUs > 0) ? (10000000u / avgUs) : 0u, level, mode, main, load, N3DS_LinearFreeBytes() / 1024ul);
}

void N3DS_DebugLinkPoll(void)
{
	const int op = __atomic_load_n(&s_op, __ATOMIC_ACQUIRE);

	s_frames++;
	if (op == LINK_OP_NONE)
	{
		return;
	}
	switch (op)
	{
	case LINK_OP_SHOT:
		DoShot();
		break;
	case LINK_OP_STATE:
		DoState();
		break;
	case LINK_OP_SHOTR:
		DoShotRight();
		break;
	case LINK_OP_POKE:
		memcpy((void *)(uintptr_t)s_pokeAddr, s_pokeBytes, s_pokeLen);
		snprintf(s_result, sizeof(s_result), "200 wrote %u byte(s) at %08x", s_pokeLen, s_pokeAddr);
		break;
	case LINK_OP_MARK:
	{
		char line[120];

		snprintf(line, sizeof(line), "[MARK] frame=%u %s", s_frames, s_arg);
		N3DS_DebugLog(line);
		N3DS_LogFlushNow();
		snprintf(s_result, sizeof(s_result), "200 marked at frame %u", s_frames);
		break;
	}
	default:
		snprintf(s_result, sizeof(s_result), "500 unknown op");
		break;
	}
	__atomic_store_n(&s_op, LINK_OP_NONE, __ATOMIC_RELEASE);
	LightEvent_Signal(&s_done);
}

#endif /* CTR_INTERNAL */
