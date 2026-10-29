/*
 * Touch-driven debug/info UI on the bottom screen (New 3DS port).
 *
 * Open/close: SELECT + D-PAD DOWN (any build). Tabs: INFO, PERF (any build) and
 * DEBUG (CTR_INTERNAL builds only). While it is open the bottom screen shows
 * this UI instead of the minimap / stats console; closed, the bottom screen is
 * exactly as before and the only cost is one mask test per input poll.
 *
 * Like n3ds_backend.h, this header exposes plain scalars only (the game's
 * unity TU includes it, and cannot see libctru).
 */
#ifndef N3DS_DEBUGUI_H
#define N3DS_DEBUGUI_H

/* Nonzero while the UI owns the bottom screen. Read by the backend's bottom
 * screen paths and by the renderer's per-frame hook. */
extern volatile int g_n3dsDebugUiOpen;

/* Game-side numbers the libctru TU cannot see; filled by the renderer TU. */
struct N3DS_DebugUIGameInfo
{
	unsigned int drawCalls;   /* cumulative C3D draw calls */
	unsigned int mergeVerts;  /* cumulative vertices through the merge path */
	int levelID;              /* track / level id (NativePerf_CurrentLevelID) */
	int gameMode;             /* gGT->gameMode1 */
	int mainState;            /* sdata->mainGameState */
	int perfValid;            /* [PERF] window numbers below are meaningful */
	int perfTotalUs;          /* last 64-frame window, avg frame */
	int perfWorkUs;
	int perfWorstUs;
	const char *bucketName[2];
	int bucketUs[2];
};

/* Main thread, from N3DS_ReadButtons. */
void N3DS_DebugUI_Toggle(void);
void N3DS_DebugUI_Touch(void);

/* Main thread, once per presented frame, ONLY while g_n3dsDebugUiOpen:
 * records the frame time, redraws if due. Call before N3DS_GfxPresent. */
void N3DS_DebugUI_Tick(const struct N3DS_DebugUIGameInfo *gi);

#endif
