/*
 * The bottom screen's UI (New 3DS port): tabs ESTADO / MAPA / OPCIONES /
 * ACTUALIZADOR (+ DEBUG in CTR_INTERNAL builds), touch driven, up from the
 * very first frame.
 *
 * ONE OWNER. Nothing else writes the bottom framebuffer: there is no libctru
 * console on it (consoleInit would force RGB565 + single buffering, see
 * N3DS_GfxInit), the format is fixed to BGR8 and the buffering to double, and
 * once the game renders, the bottom citro3d target is the only thing that
 * presents it. The UI is rasterised on the CPU into a 320x240 RGBA5551 picture,
 * Morton-tiled into a 512x256 texture, and drawn as one textured quad as the
 * first thing of the bottom pass (N3DS_BottomUiDrawQuad, n3ds_backend.c); the
 * minimap, when its tab is up, is drawn over it in the content area. Before the
 * first GPU frame (asset loading) the same picture is blitted to the
 * framebuffer on the CPU -- the only moment the CPU touches it, and it ends
 * for good at the first GPU pass (N3DS_BottomUi_NoteGpuOwned).
 *
 * Cost: repaint ONLY on change (a touch, a state change, <= 5 Hz for live
 * numbers while their tab is up); nothing at all when idle. Like
 * n3ds_backend.h this header exposes plain scalars only (the game's unity TU
 * includes it and cannot see libctru).
 */
#ifndef N3DS_BOTTOMUI_H
#define N3DS_BOTTOMUI_H

/* The texture the quad samples: 512x256 RGBA5551, Morton-tiled in 8x8 tiles. */
#define N3DS_BOTTOM_UI_TEX_W 512
#define N3DS_BOTTOM_UI_TEX_H 256
/* Tab bar height in panel pixels (the panel is 320x240); the content area is
 * the rest. The minimap's viewport is the content area. */
#define N3DS_BOTTOM_TAB_H 28
#define N3DS_BOTTOM_CONTENT_H (240 - N3DS_BOTTOM_TAB_H)

/* Game-side numbers the libctru TU cannot see; filled by the renderer TU. */
struct N3DS_BottomUiGameInfo
{
	unsigned int drawCalls;  /* cumulative C3D draw calls */
	unsigned int mergeVerts; /* cumulative vertices through the merge path */
	int levelID;             /* track / level id (NativePerf_CurrentLevelID) */
	int gameMode;            /* gGT->gameMode1 */
	int mainState;           /* sdata->mainGameState */
	int perfValid;           /* [PERF] window numbers below are meaningful */
	int perfTotalUs;         /* last 64-frame window, avg frame */
	int perfWorkUs;
	int perfWorstUs;
	const char *bucketName[2];
	int bucketUs[2];
};

/* N3DS_GfxInit, right after gfxInitDefault: fixes the format/buffering and
 * puts the first picture on the panel (CPU). */
void N3DS_BottomUi_Init(void);

/* Main thread, from N3DS_ReadButtons: the touch poll. */
void N3DS_BottomUi_Touch(void);

/* Debug link: the bottom panel as it is painted (320x240 BGR, row-major); 0 when
 * there is no picture yet. And the mean frame time of the last 120 frames. */
int N3DS_BottomUi_Snapshot(unsigned char *bgr);
unsigned N3DS_BottomUi_AvgFrameUs(void);
/* The Debug tab's one-line status message (the CAPTURE result). */
void N3DS_BottomUi_SetMessage(const char *msg);

/* Main thread, once per presented frame (renderer SwapWindow): frame-time
 * history, live-number refresh scheduling, FTP reaping. */
void N3DS_BottomUi_Frame(const struct N3DS_BottomUiGameInfo *gi);

/* Bottom pass side (the thread that ends the frame). NeedsPaint: a new picture
 * is due. Paint: repaints and returns the tiled texture contents to upload
 * (NULL when nothing changed, unless `force`: the first upload). */
int N3DS_BottomUi_NeedsPaint(void);
const unsigned short *N3DS_BottomUi_Paint(int force);
/* The first GPU pass ran: the CPU never blits the panel again. */
void N3DS_BottomUi_NoteGpuOwned(void);

/* MAPA is the selected tab: the minimap (and the FPS overlay) is drawn. */
int N3DS_BottomUi_MapTabActive(void);
int N3DS_BottomUi_GpuOwned(void); /* the GPU draws the bottom screen: its pixels are only in the framebuffer */
int N3DS_BottomUi_HudBelow(void); /* HUD AUTO-HIDE on, MAPA tab up and a race map live */
int N3DS_BottomUi_HudBelowWanted(void); /* HUD AUTO-HIDE on and the MAPA tab up (a routed HUD triangle proves the HUD exists) */
/* The pass tells whether it drew a live minimap this frame (else the MAPA tab
 * says there is none); also auto-selects MAPA when a race map first appears. */
void N3DS_BottomUi_MapLive(int live);

/* Any thread: a log line (for the DEBUG tab's log view) and the pre-game
 * "label NN%" status (the old console status row). */
void N3DS_BottomUi_LogLine(const char *text);
void N3DS_BottomUi_SetStatus(const char *text);

#endif
