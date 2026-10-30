/*
 * Derived from REDRIVER2/PsyCross MIT source:
 * externals/PsyCross/src/gte/PsyX_GTE.cpp
 * See THIRD_PARTY_NOTICES.md for copyright and license details.
 */

#include <macros.h>
#include <psx/gtereg.h>
#include <psx/libgte.h>
#include "platform/native_gte_capture.h"
#if defined(CTR_INTERNAL) && defined(__3DS__)
#include "platform/native_perf.h" /* [GTEPROF] op counters */
#endif

#ifndef CTR_NATIVE_GTE_CAPTURE
#define CTR_NATIVE_GTE_CAPTURE 0
#endif

GTERegisters gteRegs;

/* force_inline is a plain hint in CTR_NATIVE builds; the op helpers below
 * must fold into each constant-op copy, so this file forces it. */
#define GTE_INLINE static inline __attribute__((always_inline))

#define GTE_SF(op)    ((op >> 19) & 1)
#define GTE_MX(op)    ((op >> 17) & 3)
#define GTE_V(op)     ((op >> 15) & 3)
#define GTE_CV(op)    ((op >> 13) & 3)
#define GTE_LM(op)    ((op >> 10) & 1)
#define GTE_FUNCT(op) (op & 63)

#define gteop(code)   (code & 0x1ffffff)

#define VX(n)         (n < 3 ? gteRegs.CP2D.p[n << 1].sw.l : C2_IR1)
#define VY(n)         (n < 3 ? gteRegs.CP2D.p[n << 1].sw.h : C2_IR2)
#define VZ(n)         (n < 3 ? gteRegs.CP2D.p[(n << 1) + 1].sw.l : C2_IR3)
#define MX11(n)       (n < 3 ? gteRegs.CP2C.p[(n << 3)].sw.l : -C2_R << 4)
#define MX12(n)       (n < 3 ? gteRegs.CP2C.p[(n << 3)].sw.h : C2_R << 4)
#define MX13(n)       (n < 3 ? gteRegs.CP2C.p[(n << 3) + 1].sw.l : C2_IR0)
#define MX21(n)       (n < 3 ? gteRegs.CP2C.p[(n << 3) + 1].sw.h : C2_R13)
#define MX22(n)       (n < 3 ? gteRegs.CP2C.p[(n << 3) + 2].sw.l : C2_R13)
#define MX23(n)       (n < 3 ? gteRegs.CP2C.p[(n << 3) + 2].sw.h : C2_R13)
#define MX31(n)       (n < 3 ? gteRegs.CP2C.p[(n << 3) + 3].sw.l : C2_R22)
#define MX32(n)       (n < 3 ? gteRegs.CP2C.p[(n << 3) + 3].sw.h : C2_R22)
#define MX33(n)       (n < 3 ? gteRegs.CP2C.p[(n << 3) + 4].sw.l : C2_R22)
#define CV1(n)        (n < 3 ? gteRegs.CP2C.p[(n << 3) + 5].sd : 0)
#define CV2(n)        (n < 3 ? gteRegs.CP2C.p[(n << 3) + 6].sd : 0)
#define CV3(n)        (n < 3 ? gteRegs.CP2C.p[(n << 3) + 7].sd : 0)

/*
 * Speed notes (Old 3DS: 268 MHz ARM11, no L2). Every op of the software GTE
 * used to go through GTE_operator's switch and read the op's shift flag back
 * from a global between helpers the compiler could not inline; the hot ops
 * are issued with constant op words (the gte_* macros), so doCOP2 now sends
 * those to one straight-line copy each (GTE_Op*, below), where sf and lm are
 * constants and the helpers inline. The arithmetic -- every expression, every
 * FLAG bit, the PsyCross quirks included -- is the original's, unchanged:
 * the only edits are sf passed as an argument instead of the m_sf global, the
 * MAC0/MAC3 intermediates kept in locals instead of m_mac0/m_mac3 (both were
 * only ever read inside the op that wrote them), and the count of leading
 * zeros done with CLZ. GTECapture_NoteMatrix runs once per projecting op
 * instead of once per vertex: the matrix cannot change between the vertices
 * of one RTPT, so the second and third calls always returned early.
 */

u32 gte_leadingzerocount(u32 lzcs)
{
	u32 lzcr = ((s32)lzcs < 0) ? ~lzcs : lzcs;

	if (!lzcr)
	{
		return 32;
	}

	return (u32)__builtin_clz(lzcr);
}

GTE_INLINE int GTE_Lim(int value, int max, int min, unsigned int flag)
{
	if (value > max)
	{
		C2_FLAG |= flag;
		return max;
	}
	else if (value < min)
	{
		C2_FLAG |= flag;
		return min;
	}

	return value;
}

/* Out-of-line twin for MFC2's IRGB read (native_inline_c.c, gtereg.h). */
int LIM(int value, int max, int min, unsigned int flag)
{
	return GTE_Lim(value, max, min, flag);
}

GTE_INLINE s64 gte_shift(s64 a, int sf)
{
	if (sf > 0)
	{
		return a >> 12;
	}
	else if (sf < 0)
	{
		return a << 12;
	}

	return a;
}

GTE_INLINE int BOUNDS(/*int44*/ s64 value, int max_flag, int min_flag, int sf)
{
	if (value /*.positive_overflow()*/ > (s64)0x7ffffffffff)
	{
		C2_FLAG |= max_flag;
	}

	if (value /*.negative_overflow()*/ < (s64)-0x8000000000)
	{
		C2_FLAG |= min_flag;
	}

	return (int)(gte_shift(value /*.value()*/, sf));
}

GTE_INLINE u32 gte_divide(u16 numerator, u16 denominator)
{
	if (numerator < (denominator * 2))
	{
		local_persist const u8 table[] = {
		    0xff, 0xfd, 0xfb, 0xf9, 0xf7, 0xf5, 0xf3, 0xf1, 0xef, 0xee, 0xec, 0xea, 0xe8, 0xe6, 0xe4, 0xe3, 0xe1, 0xdf, 0xdd, 0xdc, 0xda, 0xd8, 0xd6, 0xd5,
		    0xd3, 0xd1, 0xd0, 0xce, 0xcd, 0xcb, 0xc9, 0xc8, 0xc6, 0xc5, 0xc3, 0xc1, 0xc0, 0xbe, 0xbd, 0xbb, 0xba, 0xb8, 0xb7, 0xb5, 0xb4, 0xb2, 0xb1, 0xb0,
		    0xae, 0xad, 0xab, 0xaa, 0xa9, 0xa7, 0xa6, 0xa4, 0xa3, 0xa2, 0xa0, 0x9f, 0x9e, 0x9c, 0x9b, 0x9a, 0x99, 0x97, 0x96, 0x95, 0x94, 0x92, 0x91, 0x90,
		    0x8f, 0x8d, 0x8c, 0x8b, 0x8a, 0x89, 0x87, 0x86, 0x85, 0x84, 0x83, 0x82, 0x81, 0x7f, 0x7e, 0x7d, 0x7c, 0x7b, 0x7a, 0x79, 0x78, 0x77, 0x75, 0x74,
		    0x73, 0x72, 0x71, 0x70, 0x6f, 0x6e, 0x6d, 0x6c, 0x6b, 0x6a, 0x69, 0x68, 0x67, 0x66, 0x65, 0x64, 0x63, 0x62, 0x61, 0x60, 0x5f, 0x5e, 0x5d, 0x5d,
		    0x5c, 0x5b, 0x5a, 0x59, 0x58, 0x57, 0x56, 0x55, 0x54, 0x53, 0x53, 0x52, 0x51, 0x50, 0x4f, 0x4e, 0x4d, 0x4d, 0x4c, 0x4b, 0x4a, 0x49, 0x48, 0x48,
		    0x47, 0x46, 0x45, 0x44, 0x43, 0x43, 0x42, 0x41, 0x40, 0x3f, 0x3f, 0x3e, 0x3d, 0x3c, 0x3c, 0x3b, 0x3a, 0x39, 0x39, 0x38, 0x37, 0x36, 0x36, 0x35,
		    0x34, 0x33, 0x33, 0x32, 0x31, 0x31, 0x30, 0x2f, 0x2e, 0x2e, 0x2d, 0x2c, 0x2c, 0x2b, 0x2a, 0x2a, 0x29, 0x28, 0x28, 0x27, 0x26, 0x26, 0x25, 0x24,
		    0x24, 0x23, 0x22, 0x22, 0x21, 0x20, 0x20, 0x1f, 0x1e, 0x1e, 0x1d, 0x1d, 0x1c, 0x1b, 0x1b, 0x1a, 0x19, 0x19, 0x18, 0x18, 0x17, 0x16, 0x16, 0x15,
		    0x15, 0x14, 0x14, 0x13, 0x12, 0x12, 0x11, 0x11, 0x10, 0x0f, 0x0f, 0x0e, 0x0e, 0x0d, 0x0d, 0x0c, 0x0c, 0x0b, 0x0a, 0x0a, 0x09, 0x09, 0x08, 0x08,
		    0x07, 0x07, 0x06, 0x06, 0x05, 0x05, 0x04, 0x04, 0x03, 0x03, 0x02, 0x02, 0x01, 0x01, 0x00, 0x00, 0x00};

		/* denominator > numerator / 2 >= 0 here, so CLZ is defined. */
		int shift = __builtin_clz((u32)denominator) - 16;

		int r1 = (denominator << shift) & 0x7fff;
		int r2 = table[((r1 + 0x40) >> 7)] + 0x101;
		int r3 = ((0x80 - (r2 * (r1 + 0x8000))) >> 8) & 0x1ffff;
		u32 reciprocal = ((r2 * r3) + 0x80) >> 8;

		return (u32)((((u64)reciprocal * (numerator << shift)) + 0x8000) >> 16);
	}

	return 0xffffffff;
}

/* Setting bits 12 & 19-22 in FLAG does not set bit 31 */

GTE_INLINE int A1(/*int44*/ s64 a, int sf)
{
	return BOUNDS(a, (1 << 31) | (1 << 30), (1 << 31) | (1 << 27), sf);
}
GTE_INLINE int A2(/*int44*/ s64 a, int sf)
{
	return BOUNDS(a, (1 << 31) | (1 << 29), (1 << 31) | (1 << 26), sf);
}
GTE_INLINE int A3(/*int44*/ s64 a, int sf)
{
	return BOUNDS(a, (1 << 31) | (1 << 28), (1 << 31) | (1 << 25), sf);
}
GTE_INLINE int Lm_B1(int a, int lm)
{
	return GTE_Lim(a, 0x7fff, -0x8000 * !lm, (1 << 31) | (1 << 24));
}
GTE_INLINE int Lm_B2(int a, int lm)
{
	return GTE_Lim(a, 0x7fff, -0x8000 * !lm, (1 << 31) | (1 << 23));
}
GTE_INLINE int Lm_B3(int a, int lm)
{
	return GTE_Lim(a, 0x7fff, -0x8000 * !lm, (1 << 22));
}

GTE_INLINE int Lm_B3_sf(s64 value, int sf, int lm)
{
	int value_sf = (int)(gte_shift(value, sf));
	int value_12 = (int)(gte_shift(value, 1));
	int max = 0x7fff;
	int min = 0;
	if (lm == 0)
	{
		min = -0x8000;
	}

	if (value_12 < -0x8000 || value_12 > 0x7fff)
	{
		C2_FLAG |= (1 << 22);
	}

	if (value_sf > max)
	{
		return max;
	}
	else if (value_sf < min)
	{
		return min;
	}

	return value_sf;
}

GTE_INLINE int Lm_C1(int a)
{
	return GTE_Lim(a, 0x00ff, 0x0000, (1 << 21));
}
GTE_INLINE int Lm_C2(int a)
{
	return GTE_Lim(a, 0x00ff, 0x0000, (1 << 20));
}
GTE_INLINE int Lm_C3(int a)
{
	return GTE_Lim(a, 0x00ff, 0x0000, (1 << 19));
}
GTE_INLINE int Lm_D(s64 a, int sf)
{
	return GTE_Lim((int)(gte_shift(a, sf)), 0xffff, 0x0000, (1 << 31) | (1 << 18));
}

GTE_INLINE u32 Lm_E(u32 result)
{
	if (result == 0xffffffff)
	{
		C2_FLAG |= (1 << 31) | (1 << 17);
		return 0x1ffff;
	}

	if (result > 0x1ffff)
	{
		return 0x1ffff;
	}

	return result;
}

GTE_INLINE s64 F(s64 a)
{
	if (a > 0x7fffffffLL)
	{
		C2_FLAG |= (1 << 31) | (1 << 16);
	}

	if (a < -0x80000000LL)
	{
		C2_FLAG |= (1 << 31) | (1 << 15);
	}

	return a;
}

GTE_INLINE int Lm_G1(s64 a)
{
	if (a > 0x3ff)
	{
		C2_FLAG |= (1 << 31) | (1 << 14);
		return 0x3ff;
	}
	if (a < -0x400)
	{
		C2_FLAG |= (1 << 31) | (1 << 14);
		return -0x400;
	}

	return (int)(a);
}

GTE_INLINE int Lm_G2(s64 a)
{
	if (a > 0x3ff)
	{
		C2_FLAG |= (1 << 31) | (1 << 13);
		return 0x3ff;
	}

	if (a < -0x400)
	{
		C2_FLAG |= (1 << 31) | (1 << 13);
		return -0x400;
	}

	return (int)(a);
}

GTE_INLINE int Lm_H(s64 value, int sf)
{
	s64 value_sf = gte_shift(value, sf);
	int value_12 = (int)(gte_shift(value, 1));
	int max = 0x1000;
	int min = 0x0000;

	if (value_sf < min || value_sf > max)
	{
		C2_FLAG |= (1 << 12);
	}

	if (value_12 > max)
	{
		return max;
	}

	if (value_12 < min)
	{
		return min;
	}

	return value_12;
}

/*
 * One vertex of RTPS / RTPT. The op has already run the capture's matrix
 * probe (GTE_NoteMatrixForOp).
 */
GTE_INLINE int GTE_RotTransPers(int idx, int lm, int sf)
{
	int h_over_sz3;
	s64 mac3;
#if defined(CTR_NATIVE) && CTR_NATIVE_GTE_CAPTURE
	s64 sxFull, syFull;
#endif

	C2_MAC1 = A1(/*int44*/ (s64)((s64)C2_TRX << 12) + (C2_R11 * VX(idx)) + (C2_R12 * VY(idx)) + (C2_R13 * VZ(idx)), sf);
	C2_MAC2 = A2(/*int44*/ (s64)((s64)C2_TRY << 12) + (C2_R21 * VX(idx)) + (C2_R22 * VY(idx)) + (C2_R23 * VZ(idx)), sf);
	mac3 = /*int44*/ (s64)((s64)C2_TRZ << 12) + (C2_R31 * VX(idx)) + (C2_R32 * VY(idx)) + (C2_R33 * VZ(idx));
	C2_MAC3 = A3(mac3, sf);
	C2_IR1 = Lm_B1(C2_MAC1, lm);
	C2_IR2 = Lm_B2(C2_MAC2, lm);
	C2_IR3 = Lm_B3_sf(mac3, sf, lm);
	C2_SZ0 = C2_SZ1;
	C2_SZ1 = C2_SZ2;
	C2_SZ2 = C2_SZ3;
	C2_SZ3 = Lm_D(mac3, 1);
	h_over_sz3 = Lm_E(gte_divide(C2_H, C2_SZ3));
	C2_SXY0 = C2_SXY1;
	C2_SXY1 = C2_SXY2;

#if defined(CTR_NATIVE) && CTR_NATIVE_GTE_CAPTURE
	// Same expressions as the retail path below, just named so the
	// pre-truncation fraction survives past the >>16.
	sxFull = F((s64)C2_OFX + ((s64)C2_IR1 * h_over_sz3));
	syFull = F((s64)C2_OFY + ((s64)C2_IR2 * h_over_sz3));
	C2_SX2 = Lm_G1(sxFull >> 16);
	C2_SY2 = Lm_G2(syFull >> 16);

	// Lm_G1/Lm_G2 themselves stay untouched; re-derive their clamp bounds
	// read-only here and skip capture on either axis clamping -- the
	// coordinate is off-screen and the fraction is meaningless.
	if (g_dbg_gteCapture && (sxFull >> 16) >= -0x400 && (sxFull >> 16) <= 0x3ff && (syFull >> 16) >= -0x400 && (syFull >> 16) <= 0x3ff)
	{
		GTECapture_Store((s16)C2_SX2, (s16)C2_SY2, sxFull, syFull, (u16)C2_SZ3, h_over_sz3, (s16)VX(idx), (s16)VY(idx), (s16)VZ(idx));
	}
#else
	C2_SX2 = Lm_G1(F((s64)C2_OFX + ((s64)C2_IR1 * h_over_sz3)) >> 16);
	C2_SY2 = Lm_G2(F((s64)C2_OFY + ((s64)C2_IR2 * h_over_sz3)) >> 16);
#endif

	return h_over_sz3;
}

/*
 * The projection every gte_rtps / gte_rtpt issues (sf=1, lm=0) as one shared
 * out-of-line copy: RTPT calls it three times rather than carrying three
 * unrolled vertices (~2 KiB each) through a 16 KiB I-cache.
 */
__attribute__((noinline)) static int GTE_RotTransPersSf1Lm0(int idx)
{
	return GTE_RotTransPers(idx, 0, 1);
}

GTE_INLINE int GTE_RotTransPersOp(int idx, int lm, int sf)
{
	if ((sf == 1) && (lm == 0))
	{
		return GTE_RotTransPersSf1Lm0(idx);
	}
	return GTE_RotTransPers(idx, lm, sf);
}

GTE_INLINE void GTE_NoteMatrixForOp(void)
{
#if defined(CTR_NATIVE) && CTR_NATIVE_GTE_CAPTURE
	// Matrix-class probe: R11..R33 + TRX/Y/Z are CP2C words 0..7, contiguous
	// (gtereg.h). One compare per projecting op tags this frame's Stores
	// with which matrix state produced them. g_dbg_gteCapture gates this and
	// the Store for the cost A/B; the test itself is paid on every side of
	// that A/B, so a measured delta is the capture work alone. State 2
	// keeps Store but drops this probe: NoteMatrix serves only the
	// matrix-class probe, while the depth model needs sz3, not mtxId.
	if (g_dbg_gteCapture == 1)
	{
		GTECapture_NoteMatrix(&gteRegs.CP2C.p[0]);
	}
#endif
}

/*
 * The whole op set. GTE_INLINE so that each GTE_Op* entry below, which
 * passes a constant op word, compiles to that one case with sf and lm folded;
 * GTE_operator is the same body for op words only known at run time.
 */
GTE_INLINE int GTE_Execute(int op)
{
	int v;
	int lm;
	int sf;
	int cv;
	int mx;
	int h_over_sz3 = 0;
	s64 mac0;

	lm = GTE_LM(gteop(op));
	sf = GTE_SF(gteop(op));

	C2_FLAG = 0;

#if defined(CTR_INTERNAL) && defined(__3DS__)
	g_gteProfOps[g_gteProfPhase][GTE_FUNCT(gteop(op))]++;
#endif

	switch (GTE_FUNCT(gteop(op)))
	{
	case 0x00:
	case 0x01:
		GTE_NoteMatrixForOp();
		h_over_sz3 = GTE_RotTransPersOp(0, lm, sf);

		mac0 = (s64)C2_DQB + ((s64)C2_DQA * h_over_sz3);
		C2_MAC0 = (int)(F(mac0));
		C2_IR0 = Lm_H(mac0, 1);

		return 1;

	case 0x06:
		C2_MAC0 = (int)(F((s64)(C2_SX0 * C2_SY1) + (C2_SX1 * C2_SY2) + (C2_SX2 * C2_SY0) - (C2_SX0 * C2_SY2) - (C2_SX1 * C2_SY0) - (C2_SX2 * C2_SY1)));
		C2_FLAG = 0;
		return 1;

	case 0x0c:

		C2_MAC1 = A1((s64)(C2_R22 * C2_IR3) - (C2_R33 * C2_IR2), sf);
		C2_MAC2 = A2((s64)(C2_R33 * C2_IR1) - (C2_R11 * C2_IR3), sf);
		C2_MAC3 = A3((s64)(C2_R11 * C2_IR2) - (C2_R22 * C2_IR1), sf);
		C2_IR1 = Lm_B1(C2_MAC1, lm);
		C2_IR2 = Lm_B2(C2_MAC2, lm);
		C2_IR3 = Lm_B3(C2_MAC3, lm);
		return 1;

	case 0x10:

		C2_MAC1 = A1((C2_R << 16) + (C2_IR0 * Lm_B1(A1(((s64)C2_RFC << 12) - (C2_R << 16), sf), 0)), sf);
		C2_MAC2 = A2((C2_G << 16) + (C2_IR0 * Lm_B2(A2(((s64)C2_GFC << 12) - (C2_G << 16), sf), 0)), sf);
		C2_MAC3 = A3((C2_B << 16) + (C2_IR0 * Lm_B3(A3(((s64)C2_BFC << 12) - (C2_B << 16), sf), 0)), sf);
		C2_IR1 = Lm_B1(C2_MAC1, lm);
		C2_IR2 = Lm_B2(C2_MAC2, lm);
		C2_IR3 = Lm_B3(C2_MAC3, lm);
		C2_RGB0 = C2_RGB1;
		C2_RGB1 = C2_RGB2;
		C2_CD2 = C2_CODE;
		C2_R2 = Lm_C1(C2_MAC1 >> 4);
		C2_G2 = Lm_C2(C2_MAC2 >> 4);
		C2_B2 = Lm_C3(C2_MAC3 >> 4);
		return 1;

	case 0x11:

		C2_MAC1 = A1((C2_IR1 << 12) + (C2_IR0 * Lm_B1(A1(((s64)C2_RFC << 12) - (C2_IR1 << 12), sf), 0)), sf);
		C2_MAC2 = A2((C2_IR2 << 12) + (C2_IR0 * Lm_B2(A2(((s64)C2_GFC << 12) - (C2_IR2 << 12), sf), 0)), sf);
		C2_MAC3 = A3((C2_IR3 << 12) + (C2_IR0 * Lm_B3(A3(((s64)C2_BFC << 12) - (C2_IR3 << 12), sf), 0)), sf);
		C2_IR1 = Lm_B1(C2_MAC1, lm);
		C2_IR2 = Lm_B2(C2_MAC2, lm);
		C2_IR3 = Lm_B3(C2_MAC3, lm);
		C2_RGB0 = C2_RGB1;
		C2_RGB1 = C2_RGB2;
		C2_CD2 = C2_CODE;
		C2_R2 = Lm_C1(C2_MAC1 >> 4);
		C2_G2 = Lm_C2(C2_MAC2 >> 4);
		C2_B2 = Lm_C3(C2_MAC3 >> 4);
		return 1;

	case 0x12:

		mx = GTE_MX(gteop(op));
		v = GTE_V(gteop(op));
		cv = GTE_CV(gteop(op));

		switch (cv)
		{
		case 2:
			C2_MAC1 = A1((s64)(MX12(mx) * VY(v)) + (MX13(mx) * VZ(v)), sf);
			C2_MAC2 = A2((s64)(MX22(mx) * VY(v)) + (MX23(mx) * VZ(v)), sf);
			C2_MAC3 = A3((s64)(MX32(mx) * VY(v)) + (MX33(mx) * VZ(v)), sf);
			Lm_B1(A1(((s64)CV1(cv) << 12) + (MX11(mx) * VX(v)), sf), 0);
			Lm_B2(A2(((s64)CV2(cv) << 12) + (MX21(mx) * VX(v)), sf), 0);
			Lm_B3(A3(((s64)CV3(cv) << 12) + (MX31(mx) * VX(v)), sf), 0);
			break;

		default:
			C2_MAC1 = A1(/*int44*/ (s64)((s64)CV1(cv) << 12) + (MX11(mx) * VX(v)) + (MX12(mx) * VY(v)) + (MX13(mx) * VZ(v)), sf);
			C2_MAC2 = A2(/*int44*/ (s64)((s64)CV2(cv) << 12) + (MX21(mx) * VX(v)) + (MX22(mx) * VY(v)) + (MX23(mx) * VZ(v)), sf);
			C2_MAC3 = A3(/*int44*/ (s64)((s64)CV3(cv) << 12) + (MX31(mx) * VX(v)) + (MX32(mx) * VY(v)) + (MX33(mx) * VZ(v)), sf);
			break;
		}

		C2_IR1 = Lm_B1(C2_MAC1, lm);
		C2_IR2 = Lm_B2(C2_MAC2, lm);
		C2_IR3 = Lm_B3(C2_MAC3, lm);
		return 1;

	case 0x13:

		C2_MAC1 = A1((s64)(C2_L11 * C2_VX0) + (C2_L12 * C2_VY0) + (C2_L13 * C2_VZ0), sf);
		C2_MAC2 = A2((s64)(C2_L21 * C2_VX0) + (C2_L22 * C2_VY0) + (C2_L23 * C2_VZ0), sf);
		C2_MAC3 = A3((s64)(C2_L31 * C2_VX0) + (C2_L32 * C2_VY0) + (C2_L33 * C2_VZ0), sf);
		C2_IR1 = Lm_B1(C2_MAC1, lm);
		C2_IR2 = Lm_B2(C2_MAC2, lm);
		C2_IR3 = Lm_B3(C2_MAC3, lm);
		C2_MAC1 = A1(/*int44*/ (s64)((s64)C2_RBK << 12) + (C2_LR1 * C2_IR1) + (C2_LR2 * C2_IR2) + (C2_LR3 * C2_IR3), sf);
		C2_MAC2 = A2(/*int44*/ (s64)((s64)C2_GBK << 12) + (C2_LG1 * C2_IR1) + (C2_LG2 * C2_IR2) + (C2_LG3 * C2_IR3), sf);
		C2_MAC3 = A3(/*int44*/ (s64)((s64)C2_BBK << 12) + (C2_LB1 * C2_IR1) + (C2_LB2 * C2_IR2) + (C2_LB3 * C2_IR3), sf);
		C2_IR1 = Lm_B1(C2_MAC1, lm);
		C2_IR2 = Lm_B2(C2_MAC2, lm);
		C2_IR3 = Lm_B3(C2_MAC3, lm);
		C2_MAC1 = A1(((C2_R << 4) * C2_IR1) + (C2_IR0 * Lm_B1(A1(((s64)C2_RFC << 12) - ((C2_R << 4) * C2_IR1), sf), 0)), sf);
		C2_MAC2 = A2(((C2_G << 4) * C2_IR2) + (C2_IR0 * Lm_B2(A2(((s64)C2_GFC << 12) - ((C2_G << 4) * C2_IR2), sf), 0)), sf);
		C2_MAC3 = A3(((C2_B << 4) * C2_IR3) + (C2_IR0 * Lm_B3(A3(((s64)C2_BFC << 12) - ((C2_B << 4) * C2_IR3), sf), 0)), sf);
		C2_IR1 = Lm_B1(C2_MAC1, lm);
		C2_IR2 = Lm_B2(C2_MAC2, lm);
		C2_IR3 = Lm_B3(C2_MAC3, lm);
		C2_RGB0 = C2_RGB1;
		C2_RGB1 = C2_RGB2;
		C2_CD2 = C2_CODE;
		C2_R2 = Lm_C1(C2_MAC1 >> 4);
		C2_G2 = Lm_C2(C2_MAC2 >> 4);
		C2_B2 = Lm_C3(C2_MAC3 >> 4);
		return 1;

	case 0x14:

		C2_MAC1 = A1(/*int44*/ (s64)((s64)C2_RBK << 12) + (C2_LR1 * C2_IR1) + (C2_LR2 * C2_IR2) + (C2_LR3 * C2_IR3), sf);
		C2_MAC2 = A2(/*int44*/ (s64)((s64)C2_GBK << 12) + (C2_LG1 * C2_IR1) + (C2_LG2 * C2_IR2) + (C2_LG3 * C2_IR3), sf);
		C2_MAC3 = A3(/*int44*/ (s64)((s64)C2_BBK << 12) + (C2_LB1 * C2_IR1) + (C2_LB2 * C2_IR2) + (C2_LB3 * C2_IR3), sf);
		C2_IR1 = Lm_B1(C2_MAC1, lm);
		C2_IR2 = Lm_B2(C2_MAC2, lm);
		C2_IR3 = Lm_B3(C2_MAC3, lm);
		C2_MAC1 = A1(((C2_R << 4) * C2_IR1) + (C2_IR0 * Lm_B1(A1(((s64)C2_RFC << 12) - ((C2_R << 4) * C2_IR1), sf), 0)), sf);
		C2_MAC2 = A2(((C2_G << 4) * C2_IR2) + (C2_IR0 * Lm_B2(A2(((s64)C2_GFC << 12) - ((C2_G << 4) * C2_IR2), sf), 0)), sf);
		C2_MAC3 = A3(((C2_B << 4) * C2_IR3) + (C2_IR0 * Lm_B3(A3(((s64)C2_BFC << 12) - ((C2_B << 4) * C2_IR3), sf), 0)), sf);
		C2_IR1 = Lm_B1(C2_MAC1, lm);
		C2_IR2 = Lm_B2(C2_MAC2, lm);
		C2_IR3 = Lm_B3(C2_MAC3, lm);
		C2_RGB0 = C2_RGB1;
		C2_RGB1 = C2_RGB2;
		C2_CD2 = C2_CODE;
		C2_R2 = Lm_C1(C2_MAC1 >> 4);
		C2_G2 = Lm_C2(C2_MAC2 >> 4);
		C2_B2 = Lm_C3(C2_MAC3 >> 4);
		return 1;

	case 0x16:

		for (v = 0; v < 3; v++)
		{
			C2_MAC1 = A1((s64)(C2_L11 * VX(v)) + (C2_L12 * VY(v)) + (C2_L13 * VZ(v)), sf);
			C2_MAC2 = A2((s64)(C2_L21 * VX(v)) + (C2_L22 * VY(v)) + (C2_L23 * VZ(v)), sf);
			C2_MAC3 = A3((s64)(C2_L31 * VX(v)) + (C2_L32 * VY(v)) + (C2_L33 * VZ(v)), sf);
			C2_IR1 = Lm_B1(C2_MAC1, lm);
			C2_IR2 = Lm_B2(C2_MAC2, lm);
			C2_IR3 = Lm_B3(C2_MAC3, lm);
			C2_MAC1 = A1(/*int44*/ (s64)((s64)C2_RBK << 12) + (C2_LR1 * C2_IR1) + (C2_LR2 * C2_IR2) + (C2_LR3 * C2_IR3), sf);
			C2_MAC2 = A2(/*int44*/ (s64)((s64)C2_GBK << 12) + (C2_LG1 * C2_IR1) + (C2_LG2 * C2_IR2) + (C2_LG3 * C2_IR3), sf);
			C2_MAC3 = A3(/*int44*/ (s64)((s64)C2_BBK << 12) + (C2_LB1 * C2_IR1) + (C2_LB2 * C2_IR2) + (C2_LB3 * C2_IR3), sf);
			C2_IR1 = Lm_B1(C2_MAC1, lm);
			C2_IR2 = Lm_B2(C2_MAC2, lm);
			C2_IR3 = Lm_B3(C2_MAC3, lm);
			C2_MAC1 = A1(((C2_R << 4) * C2_IR1) + (C2_IR0 * Lm_B1(A1(((s64)C2_RFC << 12) - ((C2_R << 4) * C2_IR1), sf), 0)), sf);
			C2_MAC2 = A2(((C2_G << 4) * C2_IR2) + (C2_IR0 * Lm_B2(A2(((s64)C2_GFC << 12) - ((C2_G << 4) * C2_IR2), sf), 0)), sf);
			C2_MAC3 = A3(((C2_B << 4) * C2_IR3) + (C2_IR0 * Lm_B3(A3(((s64)C2_BFC << 12) - ((C2_B << 4) * C2_IR3), sf), 0)), sf);
			C2_IR1 = Lm_B1(C2_MAC1, lm);
			C2_IR2 = Lm_B2(C2_MAC2, lm);
			C2_IR3 = Lm_B3(C2_MAC3, lm);
			C2_RGB0 = C2_RGB1;
			C2_RGB1 = C2_RGB2;
			C2_CD2 = C2_CODE;
			C2_R2 = Lm_C1(C2_MAC1 >> 4);
			C2_G2 = Lm_C2(C2_MAC2 >> 4);
			C2_B2 = Lm_C3(C2_MAC3 >> 4);
		}
		return 1;

	case 0x1b:

		C2_MAC1 = A1((s64)(C2_L11 * C2_VX0) + (C2_L12 * C2_VY0) + (C2_L13 * C2_VZ0), sf);
		C2_MAC2 = A2((s64)(C2_L21 * C2_VX0) + (C2_L22 * C2_VY0) + (C2_L23 * C2_VZ0), sf);
		C2_MAC3 = A3((s64)(C2_L31 * C2_VX0) + (C2_L32 * C2_VY0) + (C2_L33 * C2_VZ0), sf);
		C2_IR1 = Lm_B1(C2_MAC1, lm);
		C2_IR2 = Lm_B2(C2_MAC2, lm);
		C2_IR3 = Lm_B3(C2_MAC3, lm);
		C2_MAC1 = A1(/*int44*/ (s64)((s64)C2_RBK << 12) + (C2_LR1 * C2_IR1) + (C2_LR2 * C2_IR2) + (C2_LR3 * C2_IR3), sf);
		C2_MAC2 = A2(/*int44*/ (s64)((s64)C2_GBK << 12) + (C2_LG1 * C2_IR1) + (C2_LG2 * C2_IR2) + (C2_LG3 * C2_IR3), sf);
		C2_MAC3 = A3(/*int44*/ (s64)((s64)C2_BBK << 12) + (C2_LB1 * C2_IR1) + (C2_LB2 * C2_IR2) + (C2_LB3 * C2_IR3), sf);
		C2_IR1 = Lm_B1(C2_MAC1, lm);
		C2_IR2 = Lm_B2(C2_MAC2, lm);
		C2_IR3 = Lm_B3(C2_MAC3, lm);
		C2_MAC1 = A1((C2_R << 4) * C2_IR1, sf);
		C2_MAC2 = A2((C2_G << 4) * C2_IR2, sf);
		C2_MAC3 = A3((C2_B << 4) * C2_IR3, sf);
		C2_IR1 = Lm_B1(C2_MAC1, lm);
		C2_IR2 = Lm_B2(C2_MAC2, lm);
		C2_IR3 = Lm_B3(C2_MAC3, lm);
		C2_RGB0 = C2_RGB1;
		C2_RGB1 = C2_RGB2;
		C2_CD2 = C2_CODE;
		C2_R2 = Lm_C1(C2_MAC1 >> 4);
		C2_G2 = Lm_C2(C2_MAC2 >> 4);
		C2_B2 = Lm_C3(C2_MAC3 >> 4);
		return 1;

	case 0x1c:

		C2_MAC1 = A1(/*int44*/ (s64)(((s64)C2_RBK) << 12) + (C2_LR1 * C2_IR1) + (C2_LR2 * C2_IR2) + (C2_LR3 * C2_IR3), sf);
		C2_MAC2 = A2(/*int44*/ (s64)(((s64)C2_GBK) << 12) + (C2_LG1 * C2_IR1) + (C2_LG2 * C2_IR2) + (C2_LG3 * C2_IR3), sf);
		C2_MAC3 = A3(/*int44*/ (s64)(((s64)C2_BBK) << 12) + (C2_LB1 * C2_IR1) + (C2_LB2 * C2_IR2) + (C2_LB3 * C2_IR3), sf);
		C2_IR1 = Lm_B1(C2_MAC1, lm);
		C2_IR2 = Lm_B2(C2_MAC2, lm);
		C2_IR3 = Lm_B3(C2_MAC3, lm);
		C2_MAC1 = A1((C2_R << 4) * C2_IR1, sf);
		C2_MAC2 = A2((C2_G << 4) * C2_IR2, sf);
		C2_MAC3 = A3((C2_B << 4) * C2_IR3, sf);
		C2_IR1 = Lm_B1(C2_MAC1, lm);
		C2_IR2 = Lm_B2(C2_MAC2, lm);
		C2_IR3 = Lm_B3(C2_MAC3, lm);
		C2_RGB0 = C2_RGB1;
		C2_RGB1 = C2_RGB2;
		C2_CD2 = C2_CODE;
		C2_R2 = Lm_C1(C2_MAC1 >> 4);
		C2_G2 = Lm_C2(C2_MAC2 >> 4);
		C2_B2 = Lm_C3(C2_MAC3 >> 4);
		return 1;

	case 0x1e:

		C2_MAC1 = A1((s64)(C2_L11 * C2_VX0) + (C2_L12 * C2_VY0) + (C2_L13 * C2_VZ0), sf);
		C2_MAC2 = A2((s64)(C2_L21 * C2_VX0) + (C2_L22 * C2_VY0) + (C2_L23 * C2_VZ0), sf);
		C2_MAC3 = A3((s64)(C2_L31 * C2_VX0) + (C2_L32 * C2_VY0) + (C2_L33 * C2_VZ0), sf);
		C2_IR1 = Lm_B1(C2_MAC1, lm);
		C2_IR2 = Lm_B2(C2_MAC2, lm);
		C2_IR3 = Lm_B3(C2_MAC3, lm);
		C2_MAC1 = A1(/*int44*/ (s64)((s64)C2_RBK << 12) + (C2_LR1 * C2_IR1) + (C2_LR2 * C2_IR2) + (C2_LR3 * C2_IR3), sf);
		C2_MAC2 = A2(/*int44*/ (s64)((s64)C2_GBK << 12) + (C2_LG1 * C2_IR1) + (C2_LG2 * C2_IR2) + (C2_LG3 * C2_IR3), sf);
		C2_MAC3 = A3(/*int44*/ (s64)((s64)C2_BBK << 12) + (C2_LB1 * C2_IR1) + (C2_LB2 * C2_IR2) + (C2_LB3 * C2_IR3), sf);
		C2_IR1 = Lm_B1(C2_MAC1, lm);
		C2_IR2 = Lm_B2(C2_MAC2, lm);
		C2_IR3 = Lm_B3(C2_MAC3, lm);
		C2_RGB0 = C2_RGB1;
		C2_RGB1 = C2_RGB2;
		C2_CD2 = C2_CODE;
		C2_R2 = Lm_C1(C2_MAC1 >> 4);
		C2_G2 = Lm_C2(C2_MAC2 >> 4);
		C2_B2 = Lm_C3(C2_MAC3 >> 4);
		return 1;

	case 0x20:

		for (v = 0; v < 3; v++)
		{
			C2_MAC1 = A1((s64)(C2_L11 * VX(v)) + (C2_L12 * VY(v)) + (C2_L13 * VZ(v)), sf);
			C2_MAC2 = A2((s64)(C2_L21 * VX(v)) + (C2_L22 * VY(v)) + (C2_L23 * VZ(v)), sf);
			C2_MAC3 = A3((s64)(C2_L31 * VX(v)) + (C2_L32 * VY(v)) + (C2_L33 * VZ(v)), sf);
			C2_IR1 = Lm_B1(C2_MAC1, lm);
			C2_IR2 = Lm_B2(C2_MAC2, lm);
			C2_IR3 = Lm_B3(C2_MAC3, lm);
			C2_MAC1 = A1(/*int44*/ (s64)((s64)C2_RBK << 12) + (C2_LR1 * C2_IR1) + (C2_LR2 * C2_IR2) + (C2_LR3 * C2_IR3), sf);
			C2_MAC2 = A2(/*int44*/ (s64)((s64)C2_GBK << 12) + (C2_LG1 * C2_IR1) + (C2_LG2 * C2_IR2) + (C2_LG3 * C2_IR3), sf);
			C2_MAC3 = A3(/*int44*/ (s64)((s64)C2_BBK << 12) + (C2_LB1 * C2_IR1) + (C2_LB2 * C2_IR2) + (C2_LB3 * C2_IR3), sf);
			C2_IR1 = Lm_B1(C2_MAC1, lm);
			C2_IR2 = Lm_B2(C2_MAC2, lm);
			C2_IR3 = Lm_B3(C2_MAC3, lm);
			C2_RGB0 = C2_RGB1;
			C2_RGB1 = C2_RGB2;
			C2_CD2 = C2_CODE;
			C2_R2 = Lm_C1(C2_MAC1 >> 4);
			C2_G2 = Lm_C2(C2_MAC2 >> 4);
			C2_B2 = Lm_C3(C2_MAC3 >> 4);
		}
		return 1;

	case 0x28:

		C2_MAC1 = A1(C2_IR1 * C2_IR1, sf);
		C2_MAC2 = A2(C2_IR2 * C2_IR2, sf);
		C2_MAC3 = A3(C2_IR3 * C2_IR3, sf);
		C2_IR1 = Lm_B1(C2_MAC1, lm);
		C2_IR2 = Lm_B2(C2_MAC2, lm);
		C2_IR3 = Lm_B3(C2_MAC3, lm);
		return 1;

	case 0x29:

		C2_MAC1 = A1(((C2_R << 4) * C2_IR1) + (C2_IR0 * Lm_B1(A1(((s64)C2_RFC << 12) - ((C2_R << 4) * C2_IR1), sf), 0)), sf);
		C2_MAC2 = A2(((C2_G << 4) * C2_IR2) + (C2_IR0 * Lm_B2(A2(((s64)C2_GFC << 12) - ((C2_G << 4) * C2_IR2), sf), 0)), sf);
		C2_MAC3 = A3(((C2_B << 4) * C2_IR3) + (C2_IR0 * Lm_B3(A3(((s64)C2_BFC << 12) - ((C2_B << 4) * C2_IR3), sf), 0)), sf);
		C2_IR1 = Lm_B1(C2_MAC1, lm);
		C2_IR2 = Lm_B2(C2_MAC2, lm);
		C2_IR3 = Lm_B3(C2_MAC3, lm);
		C2_RGB0 = C2_RGB1;
		C2_RGB1 = C2_RGB2;
		C2_CD2 = C2_CODE;
		C2_R2 = Lm_C1(C2_MAC1 >> 4);
		C2_G2 = Lm_C2(C2_MAC2 >> 4);
		C2_B2 = Lm_C3(C2_MAC3 >> 4);
		return 1;

	case 0x2a:

		for (v = 0; v < 3; v++)
		{
			C2_MAC1 = A1((C2_R0 << 16) + (C2_IR0 * Lm_B1(A1(((s64)C2_RFC << 12) - (C2_R0 << 16), sf), 0)), sf);
			C2_MAC2 = A2((C2_G0 << 16) + (C2_IR0 * Lm_B2(A2(((s64)C2_GFC << 12) - (C2_G0 << 16), sf), 0)), sf);
			C2_MAC3 = A3((C2_B0 << 16) + (C2_IR0 * Lm_B3(A3(((s64)C2_BFC << 12) - (C2_B0 << 16), sf), 0)), sf);
			C2_IR1 = Lm_B1(C2_MAC1, lm);
			C2_IR2 = Lm_B2(C2_MAC2, lm);
			C2_IR3 = Lm_B3(C2_MAC3, lm);
			C2_RGB0 = C2_RGB1;
			C2_RGB1 = C2_RGB2;
			C2_CD2 = C2_CODE;
			C2_R2 = Lm_C1(C2_MAC1 >> 4);
			C2_G2 = Lm_C2(C2_MAC2 >> 4);
			C2_B2 = Lm_C3(C2_MAC3 >> 4);
		}
		return 1;

	case 0x2d:

		mac0 = (s64)(C2_ZSF3 * C2_SZ1) + (C2_ZSF3 * C2_SZ2) + (C2_ZSF3 * C2_SZ3);
		C2_MAC0 = (int)(F(mac0));
		C2_OTZ = Lm_D(mac0, 1);
		return 1;

	case 0x2e:

		mac0 = (s64)(C2_ZSF4 * C2_SZ0) + (C2_ZSF4 * C2_SZ1) + (C2_ZSF4 * C2_SZ2) + (C2_ZSF4 * C2_SZ3);
		C2_MAC0 = (int)(F(mac0));
		C2_OTZ = Lm_D(mac0, 1);
		return 1;

	case 0x30:

		GTE_NoteMatrixForOp();
		for (v = 0; v < 3; v++)
		{
			h_over_sz3 = GTE_RotTransPersOp(v, lm, sf);
		}

		mac0 = (s64)C2_DQB + ((s64)C2_DQA * h_over_sz3);
		C2_MAC0 = (int)(F(mac0));
		C2_IR0 = Lm_H(mac0, 1);
		return 1;

	case 0x3d:

		C2_MAC1 = A1(C2_IR0 * C2_IR1, sf);
		C2_MAC2 = A2(C2_IR0 * C2_IR2, sf);
		C2_MAC3 = A3(C2_IR0 * C2_IR3, sf);
		C2_IR1 = Lm_B1(C2_MAC1, lm);
		C2_IR2 = Lm_B2(C2_MAC2, lm);
		C2_IR3 = Lm_B3(C2_MAC3, lm);
		C2_RGB0 = C2_RGB1;
		C2_RGB1 = C2_RGB2;
		C2_CD2 = C2_CODE;
		C2_R2 = Lm_C1(C2_MAC1 >> 4);
		C2_G2 = Lm_C2(C2_MAC2 >> 4);
		C2_B2 = Lm_C3(C2_MAC3 >> 4);
		return 1;

	case 0x3e:

		C2_MAC1 = A1(gte_shift(C2_MAC1, -sf) + (C2_IR0 * C2_IR1), sf);
		C2_MAC2 = A2(gte_shift(C2_MAC2, -sf) + (C2_IR0 * C2_IR2), sf);
		C2_MAC3 = A3(gte_shift(C2_MAC3, -sf) + (C2_IR0 * C2_IR3), sf);
		C2_IR1 = Lm_B1(C2_MAC1, lm);
		C2_IR2 = Lm_B2(C2_MAC2, lm);
		C2_IR3 = Lm_B3(C2_MAC3, lm);
		C2_RGB0 = C2_RGB1;
		C2_RGB1 = C2_RGB2;
		C2_CD2 = C2_CODE;
		C2_R2 = Lm_C1(C2_MAC1 >> 4);
		C2_G2 = Lm_C2(C2_MAC2 >> 4);
		C2_B2 = Lm_C3(C2_MAC3 >> 4);
		return 1;

	case 0x3f:

		for (v = 0; v < 3; v++)
		{
			C2_MAC1 = A1((s64)(C2_L11 * VX(v)) + (C2_L12 * VY(v)) + (C2_L13 * VZ(v)), sf);
			C2_MAC2 = A2((s64)(C2_L21 * VX(v)) + (C2_L22 * VY(v)) + (C2_L23 * VZ(v)), sf);
			C2_MAC3 = A3((s64)(C2_L31 * VX(v)) + (C2_L32 * VY(v)) + (C2_L33 * VZ(v)), sf);
			C2_IR1 = Lm_B1(C2_MAC1, lm);
			C2_IR2 = Lm_B2(C2_MAC2, lm);
			C2_IR3 = Lm_B3(C2_MAC3, lm);
			C2_MAC1 = A1(/*int44*/ (s64)((s64)C2_RBK << 12) + (C2_LR1 * C2_IR1) + (C2_LR2 * C2_IR2) + (C2_LR3 * C2_IR3), sf);
			C2_MAC2 = A2(/*int44*/ (s64)((s64)C2_GBK << 12) + (C2_LG1 * C2_IR1) + (C2_LG2 * C2_IR2) + (C2_LG3 * C2_IR3), sf);
			C2_MAC3 = A3(/*int44*/ (s64)((s64)C2_BBK << 12) + (C2_LB1 * C2_IR1) + (C2_LB2 * C2_IR2) + (C2_LB3 * C2_IR3), sf);
			C2_IR1 = Lm_B1(C2_MAC1, lm);
			C2_IR2 = Lm_B2(C2_MAC2, lm);
			C2_IR3 = Lm_B3(C2_MAC3, lm);
			C2_MAC1 = A1((C2_R << 4) * C2_IR1, sf);
			C2_MAC2 = A2((C2_G << 4) * C2_IR2, sf);
			C2_MAC3 = A3((C2_B << 4) * C2_IR3, sf);
			C2_IR1 = Lm_B1(C2_MAC1, lm);
			C2_IR2 = Lm_B2(C2_MAC2, lm);
			C2_IR3 = Lm_B3(C2_MAC3, lm);
			C2_RGB0 = C2_RGB1;
			C2_RGB1 = C2_RGB2;
			C2_CD2 = C2_CODE;
			C2_R2 = Lm_C1(C2_MAC1 >> 4);
			C2_G2 = Lm_C2(C2_MAC2 >> 4);
			C2_B2 = Lm_C3(C2_MAC3 >> 4);
		}
		return 1;
	}

	return 0;
}

int GTE_operator(int op)
{
	return GTE_Execute(op);
}

/*
 * Constant-op entries: the op words the game issues most (doCOP2 in
 * native_inline_c.c calls these when the op word is a compile-time constant,
 * GTE_operator otherwise). noinline: a projection is ~1 KiB of code and is
 * issued from a hundred call sites; one shared copy per op keeps the I-cache
 * footprint of a hot loop small.
 */
#define GTE_OP_ENTRY(name, opword)               \
	__attribute__((noinline)) int name(void) \
	{                                            \
		return GTE_Execute(opword);              \
	}

GTE_OP_ENTRY(GTE_OpRTPS, 0x0180001)
GTE_OP_ENTRY(GTE_OpRTPT, 0x0280030)
GTE_OP_ENTRY(GTE_OpNCLIP, 0x1400006)
GTE_OP_ENTRY(GTE_OpAVSZ3, 0x158002D)
GTE_OP_ENTRY(GTE_OpAVSZ4, 0x168002E)
GTE_OP_ENTRY(GTE_OpDPCT, 0x0F8002A)
GTE_OP_ENTRY(GTE_OpDPCS, 0x0780010)
GTE_OP_ENTRY(GTE_OpSQR0, 0x0A00428)
GTE_OP_ENTRY(GTE_OpOP12, 0x178000C)
GTE_OP_ENTRY(GTE_OpMVMVA_RT_V0_TR, 0x0480012)   /* gte_rt / gte_rtv0tr */
GTE_OP_ENTRY(GTE_OpMVMVA_RT_V0, 0x0486012)      /* gte_rtv0, mvmva(1,0,0,3,0) */
GTE_OP_ENTRY(GTE_OpMVMVA_RT_V0_SF0, 0x0406012)  /* mvmva(0,0,0,3,0) */
GTE_OP_ENTRY(GTE_OpMVMVA_RT_IR, 0x049E012)      /* gte_rtir */
GTE_OP_ENTRY(GTE_OpMVMVA_RT_IR_SF0, 0x041E012)  /* gte_rtir_sf0 */
GTE_OP_ENTRY(GTE_OpMVMVA_LL_V0, 0x04A6012)      /* gte_llv0, mvmva(1,1,0,3,0) */
GTE_OP_ENTRY(GTE_OpMVMVA_LC_V0, 0x04C6012)      /* mvmva(1,2,0,3,0) */
GTE_OP_ENTRY(GTE_OpMVMVA_LC_IR, 0x04DE012)      /* gte_lcir */
