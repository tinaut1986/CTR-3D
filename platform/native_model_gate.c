/*
 * native_model_gate.c -- the projection gate of a native-model instance, without
 * the software GTE. include/platform/native_model_gate.h has the design; this file
 * is the arithmetic.
 *
 * Must follow native_gte_core.c in the unity TU (main.c, and the parity
 * harness's rb_tu.c): it uses that file's gte_divide, so the divide is the
 * GTE's UNR table, not a second approximation of it.
 */

#include "platform/native_model_gate.h"

#if defined(CTR_NATIVE) && defined(CTR_NATIVE_MODEL_RENDERER) && CTR_NATIVE_MODEL_RENDERER

int g_dbg_modelGate = 1;

/* native_model_emit.c, later in the unity TU. */
extern int g_dbg_modelDepthCheck;

int NativeModelGate_Begin(struct NativeModelGate *g)
{
	if (!g_dbg_modelGate || g_dbg_modelDepthCheck)
	{
		return 0;
	}

	g->r11 = C2_R11;
	g->r12 = C2_R12;
	g->r13 = C2_R13;
	g->r21 = C2_R21;
	g->r22 = C2_R22;
	g->r23 = C2_R23;
	g->r31 = C2_R31;
	g->r32 = C2_R32;
	g->r33 = C2_R33;
	g->trx = C2_TRX;
	g->try_ = C2_TRY;
	g->trz = C2_TRZ;
	g->ofx = C2_OFX;
	g->ofy = C2_OFY;
	/* C2_H is the s16 view of the register; gte_divide takes it as u16, the
	 * same conversion the RTPS call site makes. */
	g->h = (u16)C2_H;
	NativeModelGate_FromGte(&g->fifo);
	return 1;
}

/*
 * One vertex of RTPS/RTPT (native_gte_core.c's GTE_RotTransPers with sf=1,
 * lm=0), reduced to its SXY2 and SZ3. Same expressions in the same types: the
 * three MAC sums in s64 with int products, MAC1/MAC2 as the >>12 int and IR1/IR2
 * as their s16 clamp, SZ3 as the [0, 0xffff] clamp of MAC3 >> 12 (the one FLAG
 * bit the gate reads, bit 18), h/SZ3 through the UNR divide and Lm_E's 0x1ffff
 * ceiling, then OFX/OFY + IR*q >> 16 clamped to [-0x400, 0x3ff].
 */
static inline __attribute__((always_inline)) u32 NativeModelGate_Vertex(struct NativeModelGate *g, u32 xy, u32 z, u32 *outSxy)
{
	const s32 vx = (s16)(xy & 0xffffu);
	const s32 vy = (s16)(xy >> 16);
	const s32 vz = (s16)(z & 0xffffu);
	const s64 mac1 = (s64)((s64)g->trx << 12) + (g->r11 * vx) + (g->r12 * vy) + (g->r13 * vz);
	const s64 mac2 = (s64)((s64)g->try_ << 12) + (g->r21 * vx) + (g->r22 * vy) + (g->r23 * vz);
	const s64 mac3 = (s64)((s64)g->trz << 12) + (g->r31 * vx) + (g->r32 * vy) + (g->r33 * vz);
	s32 ir1 = (s32)(mac1 >> 12);
	s32 ir2 = (s32)(mac2 >> 12);
	s32 sz = (s32)(mac3 >> 12);
	u32 flag = 0;
	u32 q;
	s32 sx;
	s32 sy;

	ir1 = (ir1 > 0x7fff) ? 0x7fff : (ir1 < -0x8000) ? -0x8000 : ir1;
	ir2 = (ir2 > 0x7fff) ? 0x7fff : (ir2 < -0x8000) ? -0x8000 : ir2;
	if (sz > 0xffff)
	{
		sz = 0xffff;
		flag = 1u << 18;
	}
	else if (sz < 0)
	{
		sz = 0;
		flag = 1u << 18;
	}

	q = gte_divide(g->h, (u16)sz);
	q = (q > 0x1ffffu) ? 0x1ffffu : q; /* Lm_E: the overflow value 0xffffffff lands on the same ceiling */

	sx = (s32)(((s64)g->ofx + ((s64)ir1 * (s64)q)) >> 16);
	sy = (s32)(((s64)g->ofy + ((s64)ir2 * (s64)q)) >> 16);
	sx = (sx > 0x3ff) ? 0x3ff : (sx < -0x400) ? -0x400 : sx;
	sy = (sy > 0x3ff) ? 0x3ff : (sy < -0x400) ? -0x400 : sy;

	g->fifo.sz[0] = g->fifo.sz[1];
	g->fifo.sz[1] = g->fifo.sz[2];
	g->fifo.sz[2] = g->fifo.sz[3];
	g->fifo.sz[3] = (u16)sz;
	*outSxy = ((u32)(u16)sy << 16) | (u32)(u16)sx;
	return flag;
}

u32 NativeModelGate_Rtpt(struct NativeModelGate *g, u32 xy0, u32 z0, u32 xy1, u32 z1, u32 xy2, u32 z2)
{
	u32 flag;

	/* RTPT is three RTPS-shaped vertices pushed through the same FIFO; FLAG
	 * accumulates over the op. */
	flag = NativeModelGate_Vertex(g, xy0, z0, &g->fifo.sxy[0]);
	flag |= NativeModelGate_Vertex(g, xy1, z1, &g->fifo.sxy[1]);
	flag |= NativeModelGate_Vertex(g, xy2, z2, &g->fifo.sxy[2]);
	return flag;
}

u32 NativeModelGate_Rtps(struct NativeModelGate *g, int reuseFirstVertex, u32 xy, u32 z)
{
	u32 sxy2;
	u32 flag;

	/* RenderBucket_LoadPrimRTPS's bit-30 continuation: SXY0 -> SXY1 and
	 * SZ1 -> SZ2 before the op shifts the FIFO. */
	if (reuseFirstVertex)
	{
		g->fifo.sxy[1] = g->fifo.sxy[0];
		g->fifo.sz[2] = g->fifo.sz[1];
	}

	flag = NativeModelGate_Vertex(g, xy, z, &sxy2);
	g->fifo.sxy[0] = g->fifo.sxy[1];
	g->fifo.sxy[1] = g->fifo.sxy[2];
	g->fifo.sxy[2] = sxy2;
	return flag;
}

void NativeModelGate_ToGte(const struct NativeModelGateFifo *f)
{
	MTC2(f->sxy[0], 12);
	MTC2(f->sxy[1], 13);
	MTC2(f->sxy[2], 14);
	MTC2(f->sz[0], 16);
	MTC2(f->sz[1], 17);
	MTC2(f->sz[2], 18);
	MTC2(f->sz[3], 19);
}

void NativeModelGate_FromGte(struct NativeModelGateFifo *f)
{
	f->sxy[0] = MFC2(12);
	f->sxy[1] = MFC2(13);
	f->sxy[2] = MFC2(14);
	f->sz[0] = (u16)MFC2(16);
	f->sz[1] = (u16)MFC2(17);
	f->sz[2] = (u16)MFC2(18);
	f->sz[3] = (u16)MFC2(19);
}

#endif
