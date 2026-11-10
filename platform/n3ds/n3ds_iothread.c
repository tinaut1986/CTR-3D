/*
 * Background I/O job thread -- see n3ds_iothread.h. Its own TU because it
 * includes libctru (compiled with -fshort-enums, see CMakeLists.txt).
 */

#include <3ds.h>

#include "n3ds_iothread.h"
#include "n3ds_backend.h"

static Thread s_ioThread;
static int s_ioState; /* 0 not started, 1 up, -1 refused */
static LightEvent s_ioWork;
static LightEvent s_ioDone;
static void (*volatile s_ioFn)(void *);
static void *volatile s_ioArg;
static volatile int s_ioBusy;

static void N3DS_IoThreadMain(void *arg)
{
	(void)arg;
	for (;;)
	{
		void (*fn)(void *);

		LightEvent_Wait(&s_ioWork);
		fn = s_ioFn;
		__sync_synchronize();
		if (fn != NULL)
		{
			fn(s_ioArg);
		}
		s_ioFn = NULL;
		__sync_synchronize();
		s_ioBusy = 0;
		LightEvent_Signal(&s_ioDone);
	}
}

static int N3DS_IoThreadUp(void)
{
	if (s_ioState == 0)
	{
		s32 prio = 0x30;

		LightEvent_Init(&s_ioWork, RESET_ONESHOT);
		LightEvent_Init(&s_ioDone, RESET_STICKY);
		svcGetThreadPriority(&prio, CUR_THREAD_HANDLE);
		/* Just below the game thread on its core (-2 = the process default,
		 * core 0): above the FTP server and the log writer (0x3E-0x3F), so
		 * a read finishes within a frame or two of idle time. */
		s_ioThread = threadCreate(N3DS_IoThreadMain, NULL, 16 * 1024, (prio < 0x3D) ? (prio + 1) : 0x3D, -2, true);
		s_ioState = (s_ioThread != NULL) ? 1 : -1;
		N3DS_DebugLog((s_ioState == 1) ? "[CTR-3DS] io thread: UP" : "[CTR-3DS] io thread: NO (threadCreate refused)");
	}
	return s_ioState == 1;
}

int N3DS_IoJobSubmit(void (*fn)(void *), void *arg)
{
	if ((fn == NULL) || s_ioBusy || !N3DS_IoThreadUp())
	{
		return 0;
	}
	s_ioBusy = 1;
	LightEvent_Clear(&s_ioDone);
	s_ioArg = arg;
	__sync_synchronize();
	s_ioFn = fn;
	__sync_synchronize();
	LightEvent_Signal(&s_ioWork);
	return 1;
}

int N3DS_IoJobBusy(void)
{
	return s_ioBusy;
}

void N3DS_IoJobWait(void)
{
	while (s_ioBusy)
	{
		LightEvent_Wait(&s_ioDone);
	}
}
