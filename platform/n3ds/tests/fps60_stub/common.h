/* Host stand-in for include/common.h: just what game/MAIN/MainFps60.c reads
 * (platform/n3ds/tests/fps60_test.c). */
#ifndef COMMON_H
#define COMMON_H
#include <stddef.h>

#define CTR_NATIVE 1

#define PAUSE_ALL                  0x0000000F
#define MAIN_MENU                  0x00002000
#define GAME_CUTSCENE              0x20000000
#define END_OF_RACE                0x00200000
#define LOADING                    0x40000000
#define RENDER_FLAG_CHECKERED_FLAG 0x1000
#define LOAD_IDLE                  0
#define INTRO_RACE_TODAY           30
#define MAIN_MENU_LEVEL            39
#define ADVENTURE_GARAGE           40

struct GameTracker
{
	int timer;
	int elapsedTimeMS;
	unsigned gameMode1;
	unsigned renderFlags;
	int boolDemoMode;
	int levelID;
};

struct SData
{
	struct GameTracker *gGT;
	int frameCounter;
	struct
	{
		int stage;
	} Loading;
	void *ptrActiveMenu;
};
extern struct SData *sdata;

int LOAD_IsOpen_Podiums(void);
int Platform_LastFrameVBlanks(void);

#include "../../../../include/ctr_fps60.h"
#endif
