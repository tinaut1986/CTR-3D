/*
 * One translation unit of the RenderBucket parity test: the GTE capture
 * table, the software GTE and game/RenderBucket from ONE source tree
 * (selected by -I), with logging stubs for the native-model capture and the
 * other platform hooks. Built twice and symbol-prefixed (base_ / test_) by
 * run.sh so both live in one binary; rb_main.c drives them.
 */
#include <common.h>
void Platform_LogInstrument(const char *f, ...) { (void)f; }
int g_dbg_modelDepthCheck;
struct sData sdata_static;
u8 *gCTRNativeScratchpadBase;
int g_dbg_bboxDepthStores;
u32 emitLog[1 << 20];
int emitN;
int emitBeginRet = 1, emitSkip = 1, emitClaimMod = 3, wideMargin = 0;
static void elog(u32 v) { if (emitN < (1 << 20)) emitLog[emitN++] = v; }
int NativeModelEmit_BeginInstance(const struct InstDrawPerPlayer *idpp, const struct PushBuffer *pb, int forceSemiTrans, int specularLit,
                                  const struct Instance *inst, int ghostComposite, u32 fadeColorWord)
{
	elog(0xE1000000u | (forceSemiTrans << 4) | (specularLit << 8) | (ghostComposite << 12)); elog(fadeColorWord); elog((u32)idpp->alphaScale);
	return emitBeginRet;
}
int NativeModelEmit_Triangle(const u32 *xy, const u32 *z, const int *c, const struct TextureLayout *tex, int blend)
{
	int i; u32 h = 0;
	elog(0xE2000000u); for (i = 0; i < 3; i++) { elog(xy[i]); elog(z[i]); elog((u32)c[i]); h = h * 31 + xy[i] + z[i]; }
	elog(tex ? (u32)CTR_ReadU32LE(&tex->u0) : 0xdead); elog((u32)blend);
	return emitClaimMod ? ((h % (u32)emitClaimMod) != 0) : 1;
}
void NativeModelEmit_EndInstance(void) { elog(0xE3000000u); }
int NativeModelEmit_SkipRetailDraw(void) { return emitSkip; }
int NativeModelEmit_SpecularEnabled(void) { return 1; }
void NativeModelFrame_Sample(const struct Instance *inst, const struct ModelHeader *mh, int lodIndex) { (void)inst; (void)mh; (void)lodIndex; }
int NativeWide_CullMarginX(int rectW) { (void)rectW; return wideMargin; }
int NativeWide_MarginClassOn(int bit) { (void)bit; return 1; }
u32 NativeGpuLinks_FromHostPointer(const void *p) { return (u32)(uintptr_t)p & 0xffffff; }
#include "platform/native_gte_capture.c"
#include "platform/native_gte_core.c"
#include "platform/native_inline_c.c"
#include "game/RenderBucket/RenderBucket_QueueExecute.c"
const void *cap_buf(int i) { return &s_gteCaptureBuf[i]; }
unsigned cap_size(void) { return sizeof(s_gteCaptureBuf[0]); }
