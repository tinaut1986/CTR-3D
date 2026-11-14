#ifndef NATIVE_RENDERER_H
#define NATIVE_RENDERER_H

#include <platform/native_renderer_types.h>

int NativeRenderer_InitialiseRender(char *windowName, int width, int height, int fullscreen);
int NativeRenderer_InitialisePSX(void);
void NativeRenderer_Shutdown(void);
void NativeRenderer_ResetDevice(void);
void NativeRenderer_BeginScene(void);
void NativeRenderer_EndScene(void);
void NativeRenderer_UpdateSwapIntervalState(int swapInterval);
void NativeRenderer_SwapWindow(void);
void NativeRenderer_StoreFrameBuffer(int x, int y, int w, int h);
void NativeRenderer_PresentVRAMDisplay(void);
void NativeRenderer_PresentVRAMRect(int x, int y, int w, int h);
void NativeRenderer_SaveVRAM(const char *outputFileName, int x, int y, int width, int height, int readFromFramebuffer);
void NativeRenderer_Clear(int x, int y, int w, int h, u8 r, u8 g, u8 b);
void NativeRenderer_ClearVRAM(int x, int y, int w, int h, u8 r, u8 g, u8 b);
void NativeRenderer_CopyVRAM(u16 *src, int x, int y, int w, int h, int dstX, int dstY);
void NativeRenderer_ReadVRAM(u16 *dst, int x, int y, int dstW, int dstH);
void NativeRenderer_UpdateVRAM(void);
void NativeRenderer_ReadFramebufferDataToVRAM(void);
void NativeRenderer_DiscardFramebufferReadback(void);
int NativeRenderer_GetVRAMStateSize(void);
int NativeRenderer_CaptureVRAMState(void *dst, int dstSize);
int NativeRenderer_RestoreVRAMState(const void *src, int srcSize);
TextureID NativeRenderer_GetVRAMTexture(void);
TextureID NativeRenderer_GetWhiteTexture(void);
void NativeRenderer_SetBlendMode(BlendMode blendMode);
void NativeRenderer_SetStencilMode(int drawPrim);
void NativeRenderer_SetOffscreenState(const RECT16 *offscreenRect, const DISPENV *displayEnv, int enable);
void NativeRenderer_SetupClipMode(const RECT16 *clipRect, const DISPENV *displayEnv, int enable);
void NativeRenderer_SetTexture(TextureID texture, TexFormat texFormat);
void NativeRenderer_SetOverrideTextureSize(int width, int height);
void NativeRenderer_SetPSXTextureSemiTransPass(int pass);
void NativeRenderer_SetPSXTextureOutputSTP(int enabled);
void NativeRenderer_SetPSXDrawMaskSet(int maskSet);
void NativeRenderer_UpdateVertexBuffer(const GrVertex *vertices, int count);
void NativeRenderer_DrawTriangles(int startVertex, int triangles);
void NativeRenderer_PushDebugLabel(const char *label);
void NativeRenderer_PopDebugLabel(void);

/*
 * Clock-item feedback blur (game/Display.c DISPLAY_Blur_Main). On the 3DS its
 * POLY_FT4 tiles carry this pair in pad1/pad2, halfwords the PS1 GPU ignores,
 * and the OT walk hands them to NativeRenderer_FeedbackBlurTile instead of
 * resolving their framebuffer texture on the CPU: the 3DS renderer copies the
 * previous frame on the GPU and draws the tiles from that copy.
 */
#define NATIVE_FBBLUR_MARK1 0xB10Cu
#define NATIVE_FBBLUR_MARK2 0xC10Cu
/* One tile: destination rect in display-relative PSX coordinates (as the
 * walk's own vertices), source rect in absolute PSX VRAM coordinates. */
void NativeRenderer_FeedbackBlurTile(float dstX0, float dstY0, float dstX1, float dstY1, int srcX0, int srcY0, int srcX1, int srcY1);

/*
 * Pause screenshot tiles (game/ElimBG.c ElimBG_HandleState), same pad1/pad2
 * idiom. The screenshot is the whole presented frame drawn back as 512-wide
 * tiles in the UI OT, so in REAL widescreen it would take the UI X ramp a
 * second time; the walk pre-inverts the ramp for these tiles
 * (NativeWide_UiShiftInverseX) so the picture lands 1:1 on the panel.
 */
#define NATIVE_PAUSEBG_MARK1 0xE1B0u
#define NATIVE_PAUSEBG_MARK2 0x6B60u

#endif
