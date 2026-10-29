/*
 * RenderBucket parity: RenderBucket_Execute (DrawFunc_Normal with every
 * uncompress mode, primitive writer and setup callback) of a base tree vs the
 * tested tree, on random 8-instance scenes: compressed and raw vertex streams,
 * interpolated and not, strip/cached/reuse/color-only commands, capture modes
 * 0/1/2, native-model capture claiming some triangles. Compares prim memory
 * and the OT (pointer fields relocated between the two arenas), the
 * scratchpad, GTE registers, both GTE capture buffers and the native-model
 * call log. "rb N t" also times both (x86, a relative number only).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <time.h>
#include <common.h>
#include <psx/gtereg.h>

struct sData sdata_static;
#define SIDES 2
#define DECL(p) \
	extern u8 *p##gCTRNativeScratchpadBase; extern u32 p##emitLog[]; extern int p##emitN, p##emitBeginRet, p##emitSkip, p##emitClaimMod, p##wideMargin; \
	extern GTERegisters p##gteRegs; extern int p##g_dbg_gteCapture; \
	extern void p##RenderBucket_Execute(void *, struct PrimMem *); extern void p##GTECapture_Clear(void); \
	extern const void *p##cap_buf(int); extern unsigned p##cap_size(void); \
	void p##Unknown_8006c49c(void) {} void p##Unknown_8006c558(void) {} void p##Unknown_8006c600(void) {} \
	int p##SquareRoot0_stub(int v) { return v >> 1; } void p##MATRIX_SET_r11r12r13r14r15(void) {} \
	int p##strcmp(const char *a, const char *b) { return strcmp(a, b); } size_t p##strlen(const char *a) { return strlen(a); } \
	int p##strncmp(const char *a, const char *b, size_t n) { return strncmp(a, b, n); } \
	void *p##memcpy(void *d, const void *s, size_t n) { return memcpy(d, s, n); } void *p##memset(void *d, int c, size_t n) { return memset(d, c, n); } \
	int p##memcmp(const void *a, const void *b, size_t n) { return memcmp(a, b, n); }
DECL(base_)
DECL(test_)

static u64 g_rng = 0x9E3779B97F4A7C15ull;
static u32 rnd(void) { g_rng ^= g_rng << 13; g_rng ^= g_rng >> 7; g_rng ^= g_rng << 17; return (u32)g_rng; }

#define ARENA (1 << 20)
#define NINST 8
static u8 *arena[SIDES];
static u8 *scratch[SIDES];
#define OT_N 4096

struct World
{
	struct Instance *inst[NINST];
	struct RenderBucketEntry { struct Instance *inst; struct Instance *base; } entries[NINST + 1];
	struct PrimMem pm;
	u32 *ot;
	u8 *prim;
};
static struct World W[SIDES];

/* Build the scene once into side 0's arena with offsets, then clone byte-for-byte into side 1 with pointer relocation:
 * simpler: build twice with the same rng seed. */
static void build(int side, u64 seed, int mode)
{
	u8 *a = arena[side];
	size_t off = 0;
#define ALLOC(T, n) ((T *)(void *)(a + (off = (off + 15) & ~15u, off += (n) * sizeof(T), off - (n) * sizeof(T))))
	g_rng = seed;
	memset(a, 0, ARENA);
	struct World *w = &W[side];
	w->ot = ALLOC(u32, OT_N);
	for (int i = 0; i < OT_N; i++) w->ot[i] = 0xffffff;
	w->prim = ALLOC(u8, 256 * 1024);
	w->pm.start = w->prim; w->pm.cursor = w->prim; w->pm.end = w->prim + 256 * 1024; w->pm.guardEnd = w->prim + 250 * 1024;
	struct PushBuffer *pb = ALLOC(struct PushBuffer, 1);
	pb->rect.w = 512; pb->rect.h = 216; pb->distanceToScreen_PREV = 0x100 + (rnd() & 0xff);
	struct TextureLayout *tl = ALLOC(struct TextureLayout, 64);
	struct TextureLayout **tlp = ALLOC(struct TextureLayout *, 64);
	for (int i = 0; i < 64; i++) { u8 *b = (u8 *)&tl[i]; for (unsigned k = 0; k < sizeof(tl[i]); k++) b[k] = (u8)rnd(); tlp[i] = (rnd() % 16) ? &tl[i] : 0; }
	static const u32 prims[] = {0x8006ad88U, 0x8006ad88U, 0x8006ad88U, 0x8006ad6cU, 0x8006b968U, 0x8006ae90U, 0x8006bad0U, 0x8006c778U, 0x8006d670U};
	static const u32 setups[] = {0x8006c948U, 0x8006c948U, 0x8006c928U, 0x8006c974U, 0x8006c984U};
	for (int n = 0; n < NINST; n++)
	{
		/* Instance followed by one InstDrawPerPlayer (player 0). */
		u8 *blk = ALLOC(u8, sizeof(struct Instance) + sizeof(struct InstDrawPerPlayer) + 64);
		struct Instance *inst = (struct Instance *)blk;
		struct InstDrawPerPlayer *idpp = (struct InstDrawPerPlayer *)(blk + sizeof(struct Instance));
		struct Model *model = ALLOC(struct Model, 1);
		struct ModelHeader *mh = ALLOC(struct ModelHeader, 1);
		int ncmd = 40 + rnd() % 400, nverts = 0, ncol = 64;
		u32 *cmd = ALLOC(u32, ncmd + 4);
		u32 *colors = ALLOC(u32, ncol);
		for (int i = 0; i < ncol; i++) colors[i] = rnd() & 0xffffff;
		cmd[0] = 1 + rnd() % ncol;
		for (int i = 1; i <= ncmd; i++)
		{
			u32 flags = 0;
			u32 r = rnd() % 100;
			if (r < 12) flags |= 0x80;            /* new strip */
			if (rnd() % 6 == 0) flags |= 0x40;    /* reuse first */
			if (rnd() % 5 == 0 && i > 3) flags |= 0x04; /* cached vertex */
			if (rnd() % 2) flags |= 0x10;         /* nclip */
			if (rnd() % 2) flags |= 0x20;
			if (rnd() % 4 == 0) flags |= 0x08;
			if (rnd() % 8 == 0) flags |= 0x01 | 0x02;
			u32 si = rnd() % ((mode & 1) ? 32 : 200);
			u32 c = (flags << 24) | (si << 16) | (((rnd() % ncol)) << 9) | (rnd() % 65);
			if (rnd() % 40 == 0) c &= 0x0000ffff; /* color-only */
			if (c == 0xffffffff) c = 0x10000;
			cmd[i] = c;
			if (((c >> 16) != 0) && !(flags & 4)) nverts++;
		}
		cmd[ncmd + 1] = 0xffffffff;
		int compressed = rnd() % 2;
		int interp = rnd() % 2;
		size_t vbytes = 0x1c + (size_t)(nverts + 8) * 8;
		struct ModelFrame *mf = (struct ModelFrame *)ALLOC(u8, vbytes);
		struct ModelFrame *nf = (struct ModelFrame *)ALLOC(u8, vbytes);
		u32 *delta = compressed ? ALLOC(u32, nverts + 8) : 0;
		mf->vertexOffset = 0x1c; nf->vertexOffset = 0x1c;
		mf->pos.x = (s16)(rnd() & 0x3ff) - 0x200; mf->pos.y = (s16)(rnd() & 0x3ff) - 0x200; mf->pos.z = (s16)(rnd() & 0x3ff) - 0x200;
		nf->pos.x = (s16)(rnd() & 0x3f); nf->pos.y = (s16)(rnd() & 0x3f); nf->pos.z = (s16)(rnd() & 0x3f);
		for (size_t k = 0x1c; k < vbytes; k++) { ((u8 *)mf)[k] = (u8)rnd(); ((u8 *)nf)[k] = (u8)rnd(); }
		if (delta) for (int k = 0; k < nverts + 8; k++) delta[k] = rnd();
		mh->ptrCommandList = (u32)(uintptr_t)cmd; mh->ptrColors = colors; mh->ptrTexLayout = tlp;
		mh->ptrFrameData = mf;
		model->numHeaders = 1; model->headers = mh;
		inst->model = model;
		inst->funcPtr[0] = (void *)(uintptr_t)setups[rnd() % 5];
		inst->funcPtr[1] = (void *)(uintptr_t)prims[rnd() % 9];
		inst->colorRGBA = rnd(); inst->alphaScale = (rnd() % 3) ? 0x1000 : (s16)(rnd() & 0xfff); inst->specLightX = (s8)rnd(); inst->reflectionRGBA = rnd();
		idpp->pushBuffer = pb;
		/* mvp: scaled rotation-ish + translation so most triangles land on screen */
		{
			int s = 0x400 + (rnd() & 0x7ff);
			idpp->mvp.m[0][0] = s; idpp->mvp.m[1][1] = s; idpp->mvp.m[2][2] = s;
			idpp->mvp.m[0][1] = (s16)(rnd() & 0x3ff) - 0x200; idpp->mvp.m[1][2] = (s16)(rnd() & 0x3ff) - 0x200; idpp->mvp.m[2][0] = (s16)(rnd() & 0x3ff) - 0x200;
			idpp->mvp.t[0] = (int)(rnd() & 0x1ff) - 0x100; idpp->mvp.t[1] = (int)(rnd() & 0x1ff) - 0x100; idpp->mvp.t[2] = 0x300 + (rnd() & 0xfff);
		}
		idpp->instFlags = 0x40 | (rnd() & 0x0000ff00) | ((rnd() & 1) ? 0x2000 : 0);
		idpp->instFlags &= ~0x80;
		idpp->instFlags &= ~0x00070000; /* keep split state off: normal draw */
		idpp->alphaScale = (rnd() % 2) ? 0 : (s16)(rnd() & 0x1fff);
		idpp->ptrCurrFrame = mf; idpp->ptrNextFrame = interp ? nf : 0;
		idpp->ptrCommandList = (u32)(uintptr_t)cmd; idpp->ptrTexLayout = tlp; idpp->ptrColorLayout = (u32)(uintptr_t)colors;
		idpp->ptrDeltaArray = (int)(uintptr_t)delta;
		idpp->depthOffset[0] = rnd() % 64; idpp->depthOffset[1] = idpp->depthOffset[0] + 64 + rnd() % 512;
		idpp->mh = mh;
		idpp->otRangeNormal = (int)(uintptr_t)(w->ot + 256 + rnd() % 256);
		idpp->otRangeSecondary = (int)(uintptr_t)(w->ot + 1024 + rnd() % 256);
		idpp->unkEC = (int)0x8006a52cU; /* DrawFunc_Normal */
		idpp->unkF0 = interp ? (int)0x8006b24cU : 0; /* uncompress select */
		w->inst[n] = inst;
		w->entries[n].inst = inst; w->entries[n].base = inst;
	}
	w->entries[NINST].inst = 0;
	memset(scratch[side], 0, 0x4000);
}

static void run(int side, int capMode)
{
	if (side == 0) { base_gCTRNativeScratchpadBase = scratch[0]; base_g_dbg_gteCapture = capMode; base_emitN = 0; base_RenderBucket_Execute(W[0].entries, &W[0].pm); }
	else { test_gCTRNativeScratchpadBase = scratch[1]; test_g_dbg_gteCapture = capMode; test_emitN = 0; test_RenderBucket_Execute(W[1].entries, &W[1].pm); }
}

static double now(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec * 1e9 + t.tv_nsec; }

int main(int argc, char **argv)
{
	int trials = argc > 1 ? atoi(argv[1]) : 2000, fails = 0;
	long tris = 0;
	for (int s = 0; s < SIDES; s++) { arena[s] = aligned_alloc(64, ARENA); scratch[s] = aligned_alloc(64, 0x4000); }
	base_GTECapture_Clear(); test_GTECapture_Clear();
	for (int t = 0; t < trials && fails < 5; t++)
	{
		u64 seed = 0x1234567ull + (u64)t * 0x9E3779B97F4A7C15ull;
		int mode = t & 3, capMode = (t >> 2) % 3;
		int beginRet = (t % 7) != 0, skip = (t % 5) != 0, claimMod = t % 4, margin = (t % 3) ? 0 : 32;
		base_emitBeginRet = test_emitBeginRet = beginRet; base_emitSkip = test_emitSkip = skip; base_emitClaimMod = test_emitClaimMod = claimMod;
		base_wideMargin = test_wideMargin = margin;
		build(0, seed, mode); build(1, seed, mode);
		/* same GTE start state */
		memset(&base_gteRegs, 0, sizeof(base_gteRegs)); memset(&test_gteRegs, 0, sizeof(test_gteRegs));
		run(0, capMode); run(1, capMode);
		/* compare: arena (relative offsets are identical; pointers differ by base) -> compare prim bytes relocated */
		ptrdiff_t delta = arena[1] - arena[0];
		int bad = 0;
		if (base_emitN != test_emitN) bad |= 1;
		else for (int i = 0; i < base_emitN; i++)
		{
			u32 a = base_emitLog[i], b = test_emitLog[i];
			if (a != b && (u32)(a + (u32)delta) != b) { bad |= 2; break; }
		}
		/* arena words: equal or equal after pointer relocation */
		{
			const u32 *A = (const u32 *)arena[0], *B = (const u32 *)arena[1];
			u32 lo = (u32)(uintptr_t)arena[0], hi = lo + ARENA;
			for (int i = 0; i < ARENA / 4; i++)
			{
				if (A[i] == B[i]) continue;
				if (A[i] >= lo && A[i] < hi && A[i] + (u32)delta == B[i]) continue;
				/* OT tags hold 24-bit addresses */
				if (((A[i] + (u32)delta) & 0xffffff) == (B[i] & 0xffffff) && (A[i] >> 24) == (B[i] >> 24)) continue;
				printf(" arena word %d: %08x vs %08x\n", i, A[i], B[i]); bad |= 4; break;
			}
		}
		{
			const u32 *A = (const u32 *)scratch[0], *B = (const u32 *)scratch[1];
			u32 lo = (u32)(uintptr_t)arena[0], hi = lo + ARENA;
			for (int i = 0; i < 0x4000 / 4; i++)
			{
				if (A[i] == B[i]) continue;
				if (A[i] >= lo && A[i] < hi && A[i] + (u32)delta == B[i]) continue;
				if ((u32)(uintptr_t)&W[0] <= A[i] && A[i] < (u32)(uintptr_t)&W[1] && A[i] + (u32)((u8 *)&W[1] - (u8 *)&W[0]) == B[i]) continue;
				printf(" scratch word %d: %08x vs %08x\n", i, A[i], B[i]); bad |= 8; break;
			}
		}
		if (memcmp(&base_gteRegs, &test_gteRegs, sizeof(base_gteRegs))) bad |= 16;
		if (memcmp(base_cap_buf(0), test_cap_buf(0), base_cap_size()) || memcmp(base_cap_buf(1), test_cap_buf(1), base_cap_size())) bad |= 32;
		tris += (u8 *)W[0].pm.cursor - W[0].prim;
		if (bad) { printf("MISMATCH trial %d mode=%d cap=%d bad=%x emitN %d/%d\n", t, mode, capMode, bad, base_emitN, test_emitN); fails++; }
	}
	printf("trials=%d primBytes=%ld emit(last)=%d fails=%d\n", trials, tris, base_emitN, fails);
	if (argc > 2 && argv[2][0] == 'p')
	{
		int s = argv[2][1] == 'o' ? 0 : 1;
		for (int k = 0; k < 8; k++)
		{
			build(s, 77 + k, 0);
			for (int rep = 0; rep < 3000; rep++) { W[s].pm.cursor = W[s].prim; run(s, 1); }
		}
		return 0;
	}
	if (argc > 2)
	{
		/* timing: same scene re-executed */
		for (int s = 0; s < SIDES; s++)
		{
			double best = 1e30;
			for (int rep = 0; rep < 5; rep++)
			{
				double t0 = now();
				for (int k = 0; k < 200; k++) { build(s, 77 + k % 8, 0); }
				double tb = now() - t0;
				t0 = now();
				for (int k = 0; k < 200; k++) { build(s, 77 + k % 8, 0); run(s, 1); }
				double tr = now() - t0 - tb;
				if (tr < best) best = tr;
			}
			printf("%s: %.1f us per 8-instance scene\n", s ? "test" : "base", best / 200 / 1000);
		}
	}
	return fails != 0;
}
