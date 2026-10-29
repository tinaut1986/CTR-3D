/*
 * One translation unit of the GTE parity test: the software GTE core and its
 * register accessors from ONE source tree (selected by -I), with entry points
 * the harness calls. Built twice (base tree, tested tree) and symbol-prefixed
 * (base_ / test_) by run.sh so both live in one binary.
 */
#include <macros.h>
#include <psx/gtereg.h>
#include <psx/libgte.h>
#include <psx/inline_c.h>

int g_dbg_gteCapture;
void GTECapture_NoteMatrix(const void *words8);
void GTECapture_Store(s16 sx, s16 sy, s64 sxFull, s64 syFull, u16 sz3, s32 hOverSz3, s16 vx, s16 vy, s16 vz);

#include "platform/native_gte_core.c"
#include "platform/native_inline_c.c"

int run_generic(int op)
{
	return GTE_operator(op);
}

/* The op words the gte_* macros issue, through doCOP2 with a constant: the
 * tested tree's specialised entries, the base tree's generic path. */
int run_const(int i)
{
	switch (i)
	{
	case 0: return doCOP2(0x0180001);
	case 1: return doCOP2(0x0280030);
	case 2: return doCOP2(0x01400006);
	case 3: return doCOP2(0x0158002D);
	case 4: return doCOP2(0x0168002E);
	case 5: return doCOP2(0x0F8002A);
	case 6: return doCOP2(0x0780010);
	case 7: return doCOP2(0x0A00428);
	case 8: return doCOP2(0x0178000C);
	case 9: return doCOP2(0x0480012);
	case 10: return doCOP2(0x0486012);
	case 11: return doCOP2(0x0406012);
	case 12: return doCOP2(0x049E012);
	case 13: return doCOP2(0x041E012);
	case 14: return doCOP2(0x04A6012);
	case 15: return doCOP2(0x04C6012);
	case 16: return doCOP2(0x04DE012);
	case 17: return doCOP2(0x0108041B);
	case 18: return doCOP2(0x0C8041E);
	}
	return -1;
}

u32 acc_mfc2(int r) { return MFC2(r); }
void acc_mtc2(u32 v, int r) { MTC2(v, r); }
u32 acc_cfc2(int r) { return CFC2(r); }
void acc_ctc2(u32 v, int r) { CTC2(v, r); }
u32 acc_mfc2_c12(void) { return MFC2(12); }
u32 acc_mfc2_c17(void) { return MFC2(17); }
void acc_mtc2_c15(u32 v) { MTC2(v, 15); }
void acc_mtc2_c28(u32 v) { MTC2(v, 28); }
void acc_ctc2_c31(u32 v) { CTC2(v, 31); }
u32 lzc(u32 v) { return gte_leadingzerocount(v); }
