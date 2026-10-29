/*
 * Derived from REDRIVER2/PsyCross MIT source:
 * externals/PsyCross/src/psx/INLINE_C.C
 * See THIRD_PARTY_NOTICES.md for copyright and license details.
 */

#include <macros.h>
#include <psx/gtereg.h>
#include <psx/inline_c.h>
#include <psx/libgte.h>

extern u32 gte_leadingzerocount(u32 lzcs);

/*
 * The register accessors are always inlined: nearly every call passes a
 * constant register number, so each folds to the one case it names instead
 * of a call into a switch (one per GTE register move, several per projected
 * triangle). The definitions keep their external linkage for any caller
 * outside this unity TU.
 */
#define GTE_ACCESSOR __attribute__((always_inline)) inline

GTE_ACCESSOR u32 MFC2(s32 reg)
{
	switch (reg)
	{
	case 1:
	case 3:
	case 5:
	case 8:
	case 9:
	case 10:
	case 11:
		gteRegs.CP2D.p[reg].d = (s32)gteRegs.CP2D.p[reg].sw.l;
		break;

	case 7:
	case 16:
	case 17:
	case 18:
	case 19:
		gteRegs.CP2D.p[reg].d = (u32)gteRegs.CP2D.p[reg].w.l;
		break;

	case 15:
		gteRegs.CP2D.p[reg].d = C2_SXY2;
		break;

	case 28:
	case 29:
		gteRegs.CP2D.p[reg].d = LIM(C2_IR1 >> 7, 0x1f, 0, 0) | (LIM(C2_IR2 >> 7, 0x1f, 0, 0) << 5) | (LIM(C2_IR3 >> 7, 0x1f, 0, 0) << 10);
		break;
	}

	return gteRegs.CP2D.p[reg].d;
}

GTE_ACCESSOR s32 MFC2_S(s32 reg)
{
	// TODO(aalhendi): Confirm whether modifier reads should also use signed semantics.
	switch (reg)
	{
	case 1:
	case 3:
	case 5:
	case 8:
	case 9:
	case 10:
	case 11:
		gteRegs.CP2D.p[reg].d = (s32)gteRegs.CP2D.p[reg].sw.l;
		break;

	case 7:
	case 16:
	case 17:
	case 18:
	case 19:
		gteRegs.CP2D.p[reg].d = (u32)gteRegs.CP2D.p[reg].w.l;
		break;

	case 15:
		gteRegs.CP2D.p[reg].d = C2_SXY2;
		break;

	case 28:
	case 29:
		gteRegs.CP2D.p[reg].d = LIM(C2_IR1 >> 7, 0x1f, 0, 0) | (LIM(C2_IR2 >> 7, 0x1f, 0, 0) << 5) | (LIM(C2_IR3 >> 7, 0x1f, 0, 0) << 10);
		break;
	}

	return gteRegs.CP2D.p[reg].sd;
}

GTE_ACCESSOR void MTC2(u32 value, s32 reg)
{
	switch (reg)
	{
	case 15:
		C2_SXY0 = C2_SXY1;
		C2_SXY1 = C2_SXY2;
		C2_SXY2 = value;
		break;

	case 28:
		C2_IR1 = (value & 0x1f) << 7;
		C2_IR2 = (value & 0x3e0) << 2;
		C2_IR3 = (value & 0x7c00) >> 3;
		break;

	case 30:
		C2_LZCR = gte_leadingzerocount(value);
		break;

	case 31:
		return;
	}

	gteRegs.CP2D.p[reg].d = value;
}

GTE_ACCESSOR void MTC2_S(s32 value, s32 reg)
{
	switch (reg)
	{
	case 15:
		C2_SXY0 = C2_SXY1;
		C2_SXY1 = C2_SXY2;
		C2_SXY2 = value;
		break;

	case 28:
		C2_IR1 = (value & 0x1f) << 7;
		C2_IR2 = (value & 0x3e0) << 2;
		C2_IR3 = (value & 0x7c00) >> 3;
		break;

	case 30:
		C2_LZCR = gte_leadingzerocount(value);
		break;

	case 31:
		return;
	}

	gteRegs.CP2D.p[reg].sd = value;
}

GTE_ACCESSOR void CTC2(u32 value, s32 reg)
{
	switch (reg)
	{
	case 4:
	case 12:
	case 20:
	case 26:
	case 27:
	case 29:
	case 30:
		value = (u32)(s32)(s16)value;
		break;

	case 31:
		value = value & 0x7ffff000;
		if ((value & 0x7f87e000) != 0)
		{
			value |= 0x80000000;
		}
		break;
	}

	gteRegs.CP2C.p[reg].d = value;
}

GTE_ACCESSOR void CTC2_S(s32 value, s32 reg)
{
	switch (reg)
	{
	case 4:
	case 12:
	case 20:
	case 26:
	case 27:
	case 29:
	case 30:
		value = (s32)(s16)value;
		break;

	case 31:
		value = value & 0x7ffff000;
		if ((value & 0x7f87e000) != 0)
		{
			value |= 0x80000000;
		}
		break;
	}

	gteRegs.CP2C.p[reg].sd = value;
}

GTE_ACCESSOR u32 CFC2(s32 reg)
{
	// TODO(aalhendi): Verify CFC2 control-register edge cases against PS1 GTE behavior.

	return gteRegs.CP2C.p[reg].d;
}

GTE_ACCESSOR s32 CFC2_S(s32 reg)
{
	// TODO(aalhendi): Verify CFC2 control-register edge cases against PS1 GTE behavior.

	return gteRegs.CP2C.p[reg].sd;
}

/* native_gte_core.c's constant-op entries. */
extern int GTE_OpRTPS(void);
extern int GTE_OpRTPT(void);
extern int GTE_OpNCLIP(void);
extern int GTE_OpAVSZ3(void);
extern int GTE_OpAVSZ4(void);
extern int GTE_OpDPCT(void);
extern int GTE_OpDPCS(void);
extern int GTE_OpSQR0(void);
extern int GTE_OpOP12(void);
extern int GTE_OpMVMVA_RT_V0_TR(void);
extern int GTE_OpMVMVA_RT_V0(void);
extern int GTE_OpMVMVA_RT_V0_SF0(void);
extern int GTE_OpMVMVA_RT_IR(void);
extern int GTE_OpMVMVA_RT_IR_SF0(void);
extern int GTE_OpMVMVA_LL_V0(void);
extern int GTE_OpMVMVA_LC_V0(void);
extern int GTE_OpMVMVA_LC_IR(void);

/*
 * Inlined into every gte_* macro: an op word known at compile time goes
 * straight to its specialised copy (sf and lm folded, no dispatch), anything
 * else to the generic GTE_operator. Both run the same arithmetic.
 */
GTE_ACCESSOR s32 doCOP2(s32 op)
{
	if (__builtin_constant_p(op))
	{
		switch (op & 0x1ffffff)
		{
		case 0x0180001:
			return GTE_OpRTPS();
		case 0x0280030:
			return GTE_OpRTPT();
		case 0x1400006:
			return GTE_OpNCLIP();
		case 0x158002D:
			return GTE_OpAVSZ3();
		case 0x168002E:
			return GTE_OpAVSZ4();
		case 0x0F8002A:
			return GTE_OpDPCT();
		case 0x0780010:
			return GTE_OpDPCS();
		case 0x0A00428:
			return GTE_OpSQR0();
		case 0x178000C:
			return GTE_OpOP12();
		case 0x0480012:
			return GTE_OpMVMVA_RT_V0_TR();
		case 0x0486012:
			return GTE_OpMVMVA_RT_V0();
		case 0x0406012:
			return GTE_OpMVMVA_RT_V0_SF0();
		case 0x049E012:
			return GTE_OpMVMVA_RT_IR();
		case 0x041E012:
			return GTE_OpMVMVA_RT_IR_SF0();
		case 0x04A6012:
			return GTE_OpMVMVA_LL_V0();
		case 0x04C6012:
			return GTE_OpMVMVA_LC_V0();
		case 0x04DE012:
			return GTE_OpMVMVA_LC_IR();
		default:
			break;
		}
	}

	return GTE_operator(op);
}
