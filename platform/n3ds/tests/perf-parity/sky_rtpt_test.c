/* Host check (run.sh): NativeModelGate_RtptFlags, game/DrawSky.c's fast path
 * (dumpreq `skyfast`), vs the software GTE's RTPT (op 0x0280030, sf=1 lm=0) on
 * random register files: SXY0-2, SZ1-3 and FLAG bits 16-18 must match. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <macros.h>
#include <psx/gtereg.h>
#include <psx/libgte.h>
#include <psx/inline_c.h>

int g_dbg_gteCapture;
void GTECapture_NoteMatrix(const void *words8) { (void)words8; }
void GTECapture_Store(s16 sx, s16 sy, s64 sxFull, s64 syFull, u16 sz3, s32 hOverSz3, s16 vx, s16 vy, s16 vz) {}
int g_dbg_modelDepthCheck;

#include "platform/native_gte_core.c"
#include "platform/native_inline_c.c"
#include "platform/native_model_gate.c"

static unsigned long long s = 88172645463325252ull;
static u32 rnd(void) { s ^= s << 13; s ^= s >> 7; s ^= s << 17; return (u32)s; }
static s32 pick(int bits) { return (s32)(rnd() & ((1u << bits) - 1)) - (s32)(1u << (bits - 1)); }

int main(int argc, char **argv)
{
	long n = (argc > 1) ? atol(argv[1]) : 1000000, bad = 0, flagged = 0, clean = 0, i;
	for (i = 0; i < n; i++)
	{
		struct NativeModelGate g;
		u32 xy[3], z[3], f1, f2;
		int k, mode = rnd() % 6;
		/* rotation: 4.12 fixed, sky-like matrices plus wild ones */
		if (mode >= 4)
		{
			for (k = 0; k < 5; k++) CTC2(((u32)(u16)pick(14) << 16) | (u16)pick(14), k);
		}
		else
		{
			for (k = 0; k < 5; k++) CTC2(rnd(), k);
		}
		CTC2(0, 5); CTC2(0, 6); CTC2(0, 7);
		if (mode == 3) { CTC2(pick(20), 5); CTC2(pick(20), 6); CTC2(pick(24), 7); }
		CTC2(pick(27), 24); CTC2(pick(27), 25);           /* OFX OFY 16.16 */
		CTC2(rnd() & ((mode == 0) ? 0xffff : 0x3ff), 26); /* H */
		if (mode >= 4) { CTC2(256 << 16, 24); CTC2(120 << 16, 25); CTC2(256 + (rnd() & 255), 26); }
		CTC2(pick(16), 27); CTC2(pick(mode == 1 ? 32 : 26), 28); /* DQA DQB */
		for (k = 0; k < 3; k++)
		{
			int b = (mode == 2) ? 8 : (mode >= 4) ? 12 : 15;
			xy[k] = ((u32)(u16)pick(b + 1) << 16) | (u16)pick(b + 1);
			z[k] = (u16)pick(b + 1);
		}
		for (k = 12; k < 20; k++) MTC2(rnd(), k);
		NativeModelGate_Load(&g);
		f2 = NativeModelGate_RtptFlags(&g, xy[0], z[0], xy[1], z[1], xy[2], z[2]);
		MTC2(xy[0], 0); MTC2(z[0], 1); MTC2(xy[1], 2); MTC2(z[1], 3); MTC2(xy[2], 4); MTC2(z[2], 5);
		GTE_operator(0x0280030);
		f1 = CFC2(31);
		if ((f1 & 0x70000u) != 0) flagged++; else if (mode >= 4) clean++;
		if (((f1 & 0x70000u) != (f2 & 0x70000u)) || (MFC2(12) != g.fifo.sxy[0]) || (MFC2(13) != g.fifo.sxy[1]) || (MFC2(14) != g.fifo.sxy[2]) ||
		    ((u16)MFC2(17) != g.fifo.sz[1]) || ((u16)MFC2(18) != g.fifo.sz[2]) || ((u16)MFC2(19) != g.fifo.sz[3]))
		{
			if (bad < 5)
				printf("mismatch %ld mode %d: flag %08x vs %08x sxy %08x/%08x %08x/%08x %08x/%08x\n", i, mode, f1, f2, MFC2(12), g.fifo.sxy[0], MFC2(13),
				       g.fifo.sxy[1], MFC2(14), g.fifo.sxy[2]);
			bad++;
		}
	}
	printf("sky RTPT: trials=%ld flagged(16-18)=%ld realistic-unflagged=%ld mismatches=%ld
", n, flagged, clean, bad);
	return bad != 0;
}
