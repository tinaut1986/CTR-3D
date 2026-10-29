/*
 * GTE parity: the software GTE of a base tree vs the tested tree, on random
 * register files (run.sh builds and runs it). Compares all 64 registers,
 * return values, the effective capture matrix notes (the NoteMatrix stub has
 * the real early-out semantics, so a probe hoisted out of RTPT's vertex loop
 * compares equal) and every GTECapture_Store argument, for every function
 * code with random sf/lm/mx/v/cv bits, every constant-op entry, every
 * register accessor, and gte_leadingzerocount over all 2^32 inputs ("x").
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <macros.h>
#include <psx/gtereg.h>

extern GTERegisters base_gteRegs, test_gteRegs;
extern int base_g_dbg_gteCapture, test_g_dbg_gteCapture;
extern int base_run_generic(int op), test_run_generic(int op), base_run_const(int i), test_run_const(int i);
extern u32 base_acc_mfc2(int), test_acc_mfc2(int), base_acc_cfc2(int), test_acc_cfc2(int);
extern void base_acc_mtc2(u32, int), test_acc_mtc2(u32, int), base_acc_ctc2(u32, int), test_acc_ctc2(u32, int);
extern u32 base_lzc(u32), test_lzc(u32);
extern u32 test_acc_mfc2_c12(void), test_acc_mfc2_c17(void);
extern void test_acc_mtc2_c15(u32), test_acc_mtc2_c28(u32), test_acc_ctc2_c31(u32);

typedef struct { u32 shadow[8]; int valid; u32 changes; u32 log[4096]; int n; } Side;
static Side S[2];
static void logv(Side *s, u32 v) { if (s->n < 4096) s->log[s->n++] = v; }
static void note(Side *s, const void *w)
{
	if (s->valid && memcmp(s->shadow, w, 32) == 0) return;
	memcpy(s->shadow, w, 32); s->valid = 1; s->changes++;
	for (int i = 0; i < 8; i++) logv(s, 0xA0000000u ^ ((const u32 *)w)[i]);
}
static void store(Side *s, s16 sx, s16 sy, s64 sxF, s64 syF, u16 sz3, s32 h, s16 vx, s16 vy, s16 vz)
{
	logv(s, 0xB0000000u); logv(s, (u16)sx | ((u32)(u16)sy << 16)); logv(s, (u32)sxF); logv(s, (u32)(sxF >> 32));
	logv(s, (u32)syF); logv(s, (u32)(syF >> 32)); logv(s, sz3); logv(s, (u32)h); logv(s, (u16)vx | ((u32)(u16)vy << 16)); logv(s, (u16)vz);
}
void base_GTECapture_NoteMatrix(const void *w) { note(&S[0], w); }
void test_GTECapture_NoteMatrix(const void *w) { note(&S[1], w); }
void base_GTECapture_Store(s16 a, s16 b, s64 c, s64 d, u16 e, s32 f, s16 g, s16 h, s16 i) { store(&S[0], a, b, c, d, e, f, g, h, i); }
void test_GTECapture_Store(s16 a, s16 b, s64 c, s64 d, u16 e, s32 f, s16 g, s16 h, s16 i) { store(&S[1], a, b, c, d, e, f, g, h, i); }

static u64 g_rng = 88172645463325252ull;
static u32 rnd(void) { g_rng ^= g_rng << 13; g_rng ^= g_rng >> 7; g_rng ^= g_rng << 17; return (u32)g_rng; }
static u32 rval(void)
{
	switch (rnd() % 8)
	{
	case 0: return rnd();
	case 1: return rnd() & 0xff;
	case 2: return (u32)(s32)(s16)(rnd() & 0xffff);
	case 3: { static const u32 e[] = {0, 1, 0x7fff, 0x8000, 0xffff, 0xffff8000, 0x7fffffff, 0x80000000, 0xffffffff, 0x1000, 0xfffff000, 0x10000}; return e[rnd() % 12]; }
	case 4: return (rnd() & 0x3ff) - 0x200;
	case 5: return ((rnd() & 0xfff) - 0x800) | ((u32)((rnd() & 0xfff) - 0x800) << 16);
	case 6: return (rnd() & 0x1fff) | ((rnd() & 0x1fff) << 16);
	default: return rnd() & 0xffff;
	}
}
static void rand_regs(GTERegisters *r)
{
	u32 *w = (u32 *)r;
	for (unsigned i = 0; i < sizeof(*r) / 4; i++) w[i] = rval();
	/* Realistic projection setups part of the time. */
	if (rnd() & 1)
	{
		r->CP2C.p[26].d = (rnd() & 1) ? 0x100 : (rnd() & 0x3ff); /* H */
		r->CP2C.p[24].d = (rnd() & 0x1ff) << 16; r->CP2C.p[25].d = (rnd() & 0xff) << 16;
		r->CP2C.p[7].d = rnd() & 0x7fff; /* TRZ */
	}
}
static int cmp_state(const char *what, int op, int ro, int rn)
{
	if (memcmp(&base_gteRegs, &test_gteRegs, sizeof(base_gteRegs)) != 0 || ro != rn || S[0].n != S[1].n || memcmp(S[0].log, S[1].log, S[0].n * 4) != 0 || S[0].valid != S[1].valid || memcmp(S[0].shadow, S[1].shadow, 32))
	{
		const u32 *a = (const u32 *)&base_gteRegs, *b = (const u32 *)&test_gteRegs;
		printf("MISMATCH %s op=%08x ret %d/%d log %d/%d\n", what, op, ro, rn, S[0].n, S[1].n);
		for (unsigned i = 0; i < sizeof(base_gteRegs) / 4; i++) if (a[i] != b[i]) printf("  word %u: %08x vs %08x\n", i, a[i], b[i]);
		return 1;
	}
	return 0;
}
static const u32 kConst[] = {0x0180001, 0x0280030, 0x01400006, 0x0158002D, 0x0168002E, 0x0F8002A, 0x0780010, 0x0A00428, 0x0178000C, 0x0480012,
                             0x0486012, 0x0406012, 0x049E012, 0x041E012, 0x04A6012, 0x04C6012, 0x04DE012, 0x0108041B, 0x0C8041E};
#define N_CONST ((int)(sizeof(kConst) / sizeof(kConst[0])))
static const int kFunct[] = {0x00, 0x01, 0x06, 0x0c, 0x10, 0x11, 0x12, 0x13, 0x14, 0x16, 0x1b, 0x1c, 0x1e, 0x20, 0x28, 0x29, 0x2a, 0x2d, 0x2e, 0x30, 0x3d, 0x3e, 0x3f, 0x05};

int main(int argc, char **argv)
{
	long trials = argc > 1 ? atol(argv[1]) : 2000000;
	long fails = 0, stores = 0;
	for (long t = 0; t < trials && fails < 10; t++)
	{
		GTERegisters r;
		int mode = rnd() % 3, ro, rn;
		int useConst = rnd() & 1;
		u32 op;
		int ci = rnd() % N_CONST;
		rand_regs(&r);
		if (useConst) op = kConst[ci];
		else op = (kFunct[rnd() % 24]) | (rnd() & 0x1ffffc0);
		base_gteRegs = r; test_gteRegs = r;
		base_g_dbg_gteCapture = test_g_dbg_gteCapture = mode;
		/* same capture shadow state on both sides */
		memset(S, 0, sizeof(S));
		if (rnd() & 1) { S[0].valid = S[1].valid = 1; if (rnd() & 1) memcpy(S[0].shadow, &r.CP2C.p[0], 32); else for (int i = 0; i < 8; i++) S[0].shadow[i] = rnd(); memcpy(S[1].shadow, S[0].shadow, 32); }
		ro = useConst ? base_run_const(ci) : base_run_generic(op);
		rn = useConst ? test_run_const(ci) : test_run_generic(op);
		stores += S[0].n;
		fails += cmp_state(useConst ? "const" : "generic", op, ro, rn);
		/* constant ops through the generic entry too */
		if (useConst)
		{
			base_gteRegs = r; test_gteRegs = r; memset(S, 0, sizeof(S));
			ro = base_run_generic(op); rn = test_run_generic(op);
			fails += cmp_state("const-generic", op, ro, rn);
		}
	}
	printf("ops: %ld trials, %ld log words, %ld fails\n", trials, stores, fails);

	/* register accessors, every register */
	for (long t = 0; t < 200000 && fails < 20; t++)
	{
		GTERegisters r; u32 v = rval(); int reg = rnd() % 32, k = rnd() % 4; u32 a = 0, b = 0;
		rand_regs(&r); base_gteRegs = r; test_gteRegs = r;
		switch (k)
		{
		case 0: a = base_acc_mfc2(reg); b = test_acc_mfc2(reg); break;
		case 1: base_acc_mtc2(v, reg); test_acc_mtc2(v, reg); break;
		case 2: a = base_acc_cfc2(reg); b = test_acc_cfc2(reg); break;
		case 3: base_acc_ctc2(v, reg); test_acc_ctc2(v, reg); break;
		}
		if (a != b || memcmp(&base_gteRegs, &test_gteRegs, sizeof(r))) { printf("ACC MISMATCH k=%d reg=%d\n", k, reg); fails++; }
		base_gteRegs = r; test_gteRegs = r;
		switch (rnd() % 5)
		{
		case 0: a = base_acc_mfc2(12); b = test_acc_mfc2_c12(); break;
		case 1: a = base_acc_mfc2(17); b = test_acc_mfc2_c17(); break;
		case 2: base_acc_mtc2(v, 15); test_acc_mtc2_c15(v); break;
		case 3: base_acc_mtc2(v, 28); test_acc_mtc2_c28(v); break;
		case 4: base_acc_ctc2(v, 31); test_acc_ctc2_c31(v); break;
		}
		if (a != b || memcmp(&base_gteRegs, &test_gteRegs, sizeof(r))) { printf("ACC-CONST MISMATCH\n"); fails++; }
	}
	printf("accessors done, fails=%ld\n", fails);
	if (argc > 2)
	{
		u32 v = 0;
		do { if (base_lzc(v) != test_lzc(v)) { printf("LZC MISMATCH %08x\n", v); fails++; break; } } while (++v != 0);
		printf("lzc exhaustive done\n");
	}
	return fails != 0;
}
