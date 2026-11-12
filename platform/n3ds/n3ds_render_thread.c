/*
 * Render worker thread OS primitives -- see n3ds_render_thread.h for the
 * boundary contract. This TU
 * is compiled with -fshort-enums (libctru's ABI), like n3ds_audio_thread.c;
 * it must never include a decomp header.
 */

#include <3ds.h>
#include <stdio.h>

#include "n3ds_render_thread.h"
#include "n3ds_backend.h"

static Thread s_renderThread;
static LightLock s_queueLock;
static LightEvent s_workEvent;
static LightEvent s_doneEvent;
static int s_workerUp;
static int s_workerStuck;
static int s_workerCore = 2;

static void (*s_workerMainFn)(void);

static void N3DS_RenderThreadEntry(void *arg)
{
	(void)arg;

	s_workerMainFn();
	threadExit(0);
}

int N3DS_RenderWorkerStart(void (*workerMain)(void))
{
	bool isNew3DS;
	s32 mainPriority;

	if (s_workerUp)
	{
		/* Idempotent for the same reason the audio worker is: re-initialising
		 * a lock the worker may hold, or re-creating the thread, corrupts
		 * state and orphans the live thread. */
		return 1;
	}

	/* Primitives are real from here down regardless of whether a thread ends
	 * up existing -- the unity-TU queue calls Lock/Signal only when the
	 * worker is up, but an initialized lock is cheap insurance against that
	 * invariant ever slipping. */
	LightLock_Init(&s_queueLock);
	LightEvent_Init(&s_workEvent, RESET_STICKY);
	LightEvent_Init(&s_doneEvent, RESET_STICKY);

	isNew3DS = N3DS_ExtraCoresUsable(); /* false on an Old 3DS and in the Old-3DS simulation */
	if (!isNew3DS)
	{
		/* Old 3DS / 2DS (and the simulation): core 2 does not exist, so the
		 * worker goes to the system core, core 1, with a share of its time
		 * reserved for the application (APT_SetAppCpuTimeLimit; the exheader's
		 * MaxCpu is the ceiling). Without it the whole frame (game logic, OT
		 * walk, GPU submission) runs on core 0 and a race frame is ~35 ms
		 * against the 33.3 ms of 30 fps. The worker runs BELOW the main
		 * thread's priority: the Rosalina hard-lock bisected on the New 3DS
		 * was a higher-than-main thread on core 1 (n3ds_audio_thread.c).
		 * cfg `core1` / core1.txt = the percentage, 0 = off (the default).
		 * Measured on a 2DS at 80 %: the worker itself keeps up, but GSP and
		 * the other system services on core 1 starve (C3D_FrameEnd 0.2 ->
		 * 5-7 ms, GPU 7 -> 15 ms per frame) and core 0 slows on the shared
		 * memory, so a race ran no faster. Kept as an experiment. */
		static const int steps[] = {80, 70, 60, 50, 40, 30};
		int pct = N3DS_Core1Percent();
		Result rc;
		char msg[112];
		unsigned i;

		if (pct <= 0)
		{
			N3DS_DebugLog("[CTR-3DS] render worker: NO (Old 3DS, core 1 off) -- sync fallback");
			return 0;
		}
		rc = APT_SetAppCpuTimeLimit((u32)pct);
		for (i = 0; R_FAILED(rc) && (i < sizeof(steps) / sizeof(steps[0])); i++)
		{
			if (steps[i] < pct)
			{
				snprintf(msg, sizeof(msg), "[CTR-3DS] core 1 time limit %d%% refused (rc=0x%08lx), trying %d%%", pct, (unsigned long)rc, steps[i]);
				N3DS_DebugLog(msg);
				pct = steps[i];
				rc = APT_SetAppCpuTimeLimit((u32)pct);
			}
		}
		if (R_FAILED(rc))
		{
			snprintf(msg, sizeof(msg), "[CTR-3DS] render worker: NO (core 1 time limit %d%% refused, rc=0x%08lx) -- sync fallback", pct, (unsigned long)rc);
			N3DS_DebugLog(msg);
			return 0;
		}
		mainPriority = 0x30;
		svcGetThreadPriority(&mainPriority, CUR_THREAD_HANDLE);
		s_workerMainFn = workerMain;
		s_renderThread = threadCreate(N3DS_RenderThreadEntry, NULL, 128 * 1024, (mainPriority < 0x3E) ? (mainPriority + 1) : mainPriority, 1, false);
		s_workerUp = (s_renderThread != NULL);
		if (!s_workerUp)
		{
			APT_SetAppCpuTimeLimit(0);
		}
		s_workerCore = 1;
		snprintf(msg, sizeof(msg), s_workerUp ? "[CTR-3DS] render worker: UP (core 1, %d%% of it)" : "[CTR-3DS] render worker: NO (threadCreate refused core 1 at %d%%) -- sync fallback", pct);
		N3DS_DebugLog(msg);
		return s_workerUp;
	}

	/* Same priority as the main thread: the worker has core 2 to itself, so
	 * priority only matters if it ever migrates, which threadCreate's fixed
	 * core assignment prevents. Unlike core 1 there is no AppCpuTimeLimit
	 * gate for core 2 -- either threadCreate succeeds or it refuses, and the
	 * log line below is the whole hardware answer to "is core 2 reachable
	 * under HBL/Luma". */
	mainPriority = 0x30;
	svcGetThreadPriority(&mainPriority, CUR_THREAD_HANDLE);

	s_workerMainFn = workerMain;
	s_renderThread = threadCreate(N3DS_RenderThreadEntry, NULL, 128 * 1024, mainPriority, 2, false);

	s_workerUp = (s_renderThread != NULL);
	N3DS_DebugLog(s_workerUp ? "[CTR-3DS] render worker: UP (core 2)"
	                         : "[CTR-3DS] render worker: NO (threadCreate refused core 2) -- sync fallback");
	return s_workerUp;
}

void N3DS_RenderWorkerStopAndJoin(void)
{
	if (!s_workerUp)
	{
		return;
	}

	/* Timed: a worker inside a GPU wait that never returns (HOME took the GPU)
	 * must cost the exit 2 s, not the console. Stuck => left running, and
	 * citro3d is then not torn down under it (N3DS_RenderWorkerStuck). */
	if (threadJoin(s_renderThread, 2000000000ULL) == 0)
	{
		N3DS_ExitLog("render worker joined");
		threadFree(s_renderThread);
	}
	else
	{
		s_workerStuck = 1;
		N3DS_ExitLog("render worker did not end in 2 s, left running");
	}
	s_renderThread = NULL;
	s_workerUp = 0;
	if (s_workerCore == 1)
	{
		/* Give the system its core back (HOME menu, the next title). */
		APT_SetAppCpuTimeLimit(0);
	}
}

int N3DS_RenderWorkerCore(void)
{
	return s_workerUp ? s_workerCore : -1;
}

int N3DS_RenderWorkerStuck(void)
{
	return s_workerStuck;
}

int N3DS_RenderWorkerIsSelf(void)
{
	/* threadGetCurrent returns NULL for the main thread and a distinct handle
	 * for the audio worker, so equality against our own handle is exact. */
	return s_workerUp && (threadGetCurrent() == s_renderThread);
}

void N3DS_RenderQueueLock(void)
{
	LightLock_Lock(&s_queueLock);
}

void N3DS_RenderQueueUnlock(void)
{
	LightLock_Unlock(&s_queueLock);
}

void N3DS_RenderWorkSignal(void)
{
	LightEvent_Signal(&s_workEvent);
}

void N3DS_RenderWorkWait(void)
{
	LightEvent_Wait(&s_workEvent);
	LightEvent_Clear(&s_workEvent);
}

void N3DS_RenderDoneSignal(void)
{
	LightEvent_Signal(&s_doneEvent);
}

void N3DS_RenderDoneWait(void)
{
	LightEvent_Wait(&s_doneEvent);
	LightEvent_Clear(&s_doneEvent);
}
