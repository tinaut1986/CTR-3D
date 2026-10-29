#ifndef NATIVE_MODEL_GATE_H
#define NATIVE_MODEL_GATE_H

#include <macros.h>

// The native-model projection gate: RenderBucket_DrawFunc_Normal's per-triangle
// accept test (FLAG, NCLIP, screen window) for an instance the native model path
// REPLACES (g_dbg_modelRender state 1), without running the software GTE.
//
// Why it exists. In state 1 the GPU already transforms every model vertex
// (shaders/ctr_model.v.pica takes the model-space vertex and the instance
// matrix); the CPU still projected every vertex through the software GTE only to
// decide WHICH triangles to hand it. That projection was the full RTPS/RTPT
// emulation -- every FLAG bound, the IR/MAC/SZ register traffic, the capture
// table's matrix probe and its cache-missing Store -- plus an NCLIP and an AVSZ3
// op per triangle whose results nothing reads once the packet write is skipped.
//
// What it is. A copy of exactly the part of RTPS/RTPT the gate consumes: the
// SXY0-2 / SZ0-3 FIFO and FLAG bit 18 (SZ3 saturation), computed with the GTE's
// own integer arithmetic (the same MAC sums, the same UNR divide, the same
// clamps), so the accepted set is the retail set triangle for triangle -- the
// host parity harness (platform/n3ds/tests/perf-parity) compares the captured
// triangles of both paths. Nothing else of the GTE is written: MAC/IR/FLAG/
// MAC0/OTZ keep whatever they held, and the capture table gets no Stores (no
// packet of a claimed triangle ever looks one up).
//
// A triangle the capture DECLINES (snapshot full, an untextured face of a
// writer that draws nothing for it) still needs its retail packet: the caller
// loads the FIFO as it stood before this triangle into the GTE
// (NativeModelGate_ToGte), runs the retail projection and writer, and reads the
// FIFO back (NativeModelGate_FromGte) -- so the GTE sees the register state the
// retail walk would have had, and the gate continues from whatever the writer
// left.
//
// Runtime toggle g_dbg_modelGate (dumpreq `mdlgate`): 1 = this gate (default),
// 0 = the software GTE for every triangle, as before. Off while the [MDLDEPTH]
// check runs (it reads SZ1-3 back from the GTE).

struct NativeModelGateFifo
{
	u32 sxy[3]; // SXY0, SXY1, SXY2 -- packed as the GTE registers (sy << 16 | (u16)sx)
	u16 sz[4];  // SZ0..SZ3
};

struct NativeModelGate
{
	struct NativeModelGateFifo fifo;
	// The instance's GTE control state, read once at Begin: R11..R33, TRX/Y/Z,
	// OFX/OFY (16.16) and H. DrawFunc_Normal never reloads them mid-instance.
	s32 r11, r12, r13, r21, r22, r23, r31, r32, r33;
	s32 trx, try_, trz;
	s32 ofx, ofy;
	u16 h;
};

extern int g_dbg_modelGate;

// Reads the control state and the current FIFO out of the GTE. Returns 0 when
// the gate is off (toggle, [MDLDEPTH]); the caller then uses the GTE as before.
int NativeModelGate_Begin(struct NativeModelGate *g);

// RTPT / RTPS on the gate's FIFO. Vertices are the packed GTE V-register pairs
// the walk would MTC2 (VX | VY << 16, VZ). reuseFirstVertex is the strip's
// bit-30 continuation (SXY0 -> SXY1, SZ1 -> SZ2 before the RTPS). Return the
// op's FLAG, of which only bit 18 is computed -- the only bit the gate tests.
u32 NativeModelGate_Rtpt(struct NativeModelGate *g, u32 xy0, u32 z0, u32 xy1, u32 z1, u32 xy2, u32 z2);
u32 NativeModelGate_Rtps(struct NativeModelGate *g, int reuseFirstVertex, u32 xy, u32 z);

// NCLIP's MAC0 on the FIFO: the GTE's own expression, in int (|SX|,|SY| <= 1024).
static inline s32 NativeModelGate_Nclip(const struct NativeModelGateFifo *f)
{
	const s32 sx0 = (s16)(f->sxy[0] & 0xffffu), sy0 = (s16)(f->sxy[0] >> 16);
	const s32 sx1 = (s16)(f->sxy[1] & 0xffffu), sy1 = (s16)(f->sxy[1] >> 16);
	const s32 sx2 = (s16)(f->sxy[2] & 0xffffu), sy2 = (s16)(f->sxy[2] >> 16);

	return (sx0 * sy1) + (sx1 * sy2) + (sx2 * sy0) - (sx0 * sy2) - (sx1 * sy0) - (sx2 * sy1);
}

// FIFO <-> GTE data registers 12-14 and 16-19.
void NativeModelGate_ToGte(const struct NativeModelGateFifo *f);
void NativeModelGate_FromGte(struct NativeModelGateFifo *f);

#endif
