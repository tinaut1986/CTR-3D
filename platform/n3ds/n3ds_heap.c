/*
 * The process heaps (libctru's __system_allocateHeaps, replaced).
 *
 * Why it is replaced: libctru's default gives an installed title a FIXED 32 MiB
 * linear heap and the application heap everything else, while a .3dsx under the
 * Homebrew Launcher gets the loader's own split. This game is the opposite of a
 * typical homebrew: it needs a LARGE linear heap (see the budget below), so as
 * a CIA the default starved citro3d init ("atlas 9 of 11 FAILED", no GPU, no
 * frames) while the .3dsx worked. Sized here from what the process may commit,
 * for every launch mode alike (HBL, CIA, 124/178 MB New 3DS, 64 MB Old 3DS).
 *
 * LINEAR BUDGET (audit of every linearAlloc / C3D_TexInit / gfx buffer):
 *   gfx framebuffers (top L+R double buffered, bottom)           ~1.6 MiB
 *   citro3d command buffer (4 x 256 KiB)                          1.0 MiB
 *   frame vertex buffer (65536 x 16 B)                             1.0 MiB
 *   11 decode atlases (1024^2 RGBA5551 = 2 MiB each)              22.0 MiB
 *   8 track atlas units (512^2 RGBA5551 = 512 KiB each)            4.0 MiB
 *   VRAM display textures (4 x 128 KiB) + bottom UI texture        0.8 MiB
 *   hw-voice sample store                                          3.0 MiB
 *   ndsp PCM blocks, audio and misc (measured: ~9 MiB were in use
 *     before the atlases in the hardware log, including the above)  ~7 MiB
 *   one level's track geometry (linearAlloc per level)             ~4 MiB
 *   => REQUIRED about 48 MiB; the XA voice cache wants 32 MiB more
 *      and has a fallback (voice lines stay on the SD path).
 * Render targets live in VRAM, not here.
 *
 * Policy: linear first, up to LINEAR_WANT (required + margin + voice cache),
 * but the application heap keeps at least HEAP_FLOOR; the heap takes the rest
 * up to its address window (OS_HEAP_AREA, ~96 MiB). If even the required part
 * cannot be had (Old 3DS 64 MB), fall back to an even split and say so in the
 * log; nothing here panics while a smaller size still works.
 *
 * LEAN POLICY (legacy app region, Old 3DS): when the commit cannot hold
 * LINEAR_REQUIRED + HEAP_FLOOR, the 48 MiB heap floor (sized for the 32 MiB XA
 * voice cache) is NOT honoured; the heap gets only HEAP_LEAN (measured: the
 * New 3DS log peaks at ~16 MiB of malloc outside the voice cache, ~8 MiB in a
 * race) and linear gets the rest up to LINEAR_REQUIRED. The voice cache sizes
 * itself to the heap that is left (native_audio.c), so it shrinks or drops.
 * Unchanged for any commit >= 96 MiB (New 3DS, HBL).
 *
 * OLD-3DS SIMULATION (N3DS_HeapSimCap): the split is computed here, before the
 * SD card exists, so a flag file cannot steer it. Instead the game, once the
 * flag is read, runs this same policy on a smaller commit and parks the
 * difference in ballast blocks (never freed) so the real heap/linear free
 * space equals what the legacy mode would leave.
 */
#include <3ds.h>
#include <malloc.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

void N3DS_DebugLog(const char *line); /* n3ds_backend.c */

extern u32 __ctru_heap, __ctru_linear_heap;
extern char *fake_heap_start, *fake_heap_end;
u32 __ctru_heap_size = 0;
u32 __ctru_linear_heap_size = 0;

#define N3DS_MIB (1u << 20)
#define N3DS_LINEAR_REQUIRED (48u * N3DS_MIB)
#define N3DS_LINEAR_WANT (88u * N3DS_MIB)
#define N3DS_HEAP_FLOOR (48u * N3DS_MIB)
#define N3DS_HEAP_MIN (24u * N3DS_MIB)
#define N3DS_LINEAR_MIN (8u * N3DS_MIB)
#define N3DS_HEAP_STEP (4u * N3DS_MIB)
#define N3DS_HEAP_LEAN (20u * N3DS_MIB)
#define N3DS_LINEAR_VIABLE (36u * N3DS_MIB)

static u32 s_commitRemaining = 0;
static u32 s_simBallastHeap = 0;
static u32 s_simBallastLinear = 0;

/* Linear share of a commit of `remaining` bytes (see the policy above). Pure:
 * the Old-3DS simulation replays it on a smaller commit. */
static u32 N3DS_LinearPlan(u32 remaining)
{
	const u32 pageMask = 0xFFF;
	u32 linear = N3DS_LINEAR_WANT;

	if (remaining > N3DS_HEAP_FLOOR)
	{
		if (linear > remaining - N3DS_HEAP_FLOOR)
		{
			linear = remaining - N3DS_HEAP_FLOOR;
		}
	}
	else
	{
		linear = 0;
	}
	if (linear < N3DS_LINEAR_REQUIRED)
	{
		/* Cannot give the renderer what it needs AND keep the heap floor
		 * (Old 3DS). Lean: heap down to HEAP_LEAN, linear up to REQUIRED. */
		if (remaining > N3DS_HEAP_LEAN + N3DS_LINEAR_VIABLE)
		{
			linear = remaining - N3DS_HEAP_LEAN;
			if (linear > N3DS_LINEAR_REQUIRED)
			{
				linear = N3DS_LINEAR_REQUIRED;
			}
		}
		else
		{
			/* Not even lean fits: even split, the game logs what does not. */
			linear = remaining / 2;
		}
	}
	return linear & ~pageMask;
}

static u32 N3DS_HeapPlan(u32 remaining, u32 linear)
{
	const u32 heapMax = (OS_HEAP_AREA_END - OS_HEAP_AREA_BEGIN) - (4u * N3DS_MIB);
	u32 heap = remaining - linear;

	if (heap > heapMax)
	{
		heap = heapMax;
	}
	return heap & ~0xFFFu;
}

void __system_allocateHeaps(void)
{
	const u32 pageMask = 0xFFF;
	Handle limit = 0;
	s64 maxCommit = 0, curCommit = 0;
	ResourceLimitType type = RESLIMIT_COMMIT;
	u32 remaining, linear, heap;

	if (R_FAILED(svcGetResourceLimit(&limit, CUR_PROCESS_HANDLE)))
	{
		svcBreak(USERBREAK_PANIC);
	}
	svcGetResourceLimitLimitValues(&maxCommit, limit, &type, 1);
	svcGetResourceLimitCurrentValues(&curCommit, limit, &type, 1);
	svcCloseHandle(limit);
	remaining = (u32)(maxCommit - curCommit) & ~pageMask;
	s_commitRemaining = remaining;
	if (remaining <= N3DS_HEAP_MIN + N3DS_LINEAR_MIN)
	{
		svcBreak(USERBREAK_PANIC);
	}

	linear = N3DS_LinearPlan(remaining);
	linear &= ~pageMask;

	/* Heap: everything else, within its address window. */
	heap = N3DS_HeapPlan(remaining, linear);
	for (;;)
	{
		__ctru_heap = 0;
		if (R_SUCCEEDED(svcControlMemory(&__ctru_heap, OS_HEAP_AREA_BEGIN, 0, heap, MEMOP_ALLOC, (MemPerm)(MEMPERM_READ | MEMPERM_WRITE))))
		{
			break;
		}
		if (heap < N3DS_HEAP_MIN + N3DS_HEAP_STEP)
		{
			svcBreak(USERBREAK_PANIC);
		}
		heap -= N3DS_HEAP_STEP;
	}
	__ctru_heap_size = heap;

	/* Whatever the heap did not take (cap, or a smaller retry) goes to linear. */
	if (remaining - heap > linear)
	{
		linear = remaining - heap;
	}
	if (linear > N3DS_LINEAR_WANT)
	{
		linear = N3DS_LINEAR_WANT;
	}
	linear &= ~pageMask;
	for (;;)
	{
		__ctru_linear_heap = 0;
		if (R_SUCCEEDED(svcControlMemory(&__ctru_linear_heap, 0, 0, linear, MEMOP_ALLOC_LINEAR, (MemPerm)(MEMPERM_READ | MEMPERM_WRITE))))
		{
			break;
		}
		/* Contiguous linear memory can be scarcer than the commit count says. */
		if (linear <= N3DS_LINEAR_MIN + N3DS_HEAP_STEP)
		{
			svcBreak(USERBREAK_PANIC);
		}
		linear -= N3DS_HEAP_STEP;
	}
	__ctru_linear_heap_size = linear;

	mappableInit(OS_MAP_AREA_BEGIN, OS_MAP_AREA_END);
	fake_heap_start = (char *)__ctru_heap;
	fake_heap_end = fake_heap_start + __ctru_heap_size;
}

/* One line for the boot log: what was chosen, from what, against what need. */
void N3DS_HeapDescribe(char *out, unsigned cap)
{
	snprintf(out, cap, "[CTR-3DS] heaps: app heap %lu KiB, linear %lu KiB (commit left %lu KiB; linear need ~%lu KiB%s)",
	         (unsigned long)(__ctru_heap_size / 1024), (unsigned long)(__ctru_linear_heap_size / 1024),
	         (unsigned long)(s_commitRemaining / 1024), (unsigned long)(N3DS_LINEAR_REQUIRED / 1024),
	         (__ctru_linear_heap_size < N3DS_LINEAR_REQUIRED) ? " -- TOO SMALL, expect failures" : "");
}

unsigned long N3DS_LinearHeapSize(void)
{
	return (unsigned long)__ctru_linear_heap_size;
}

/* ------------------------------------------------- memory accounting / sim --- */

static unsigned long s_arenaPeak = 0;
static unsigned long s_linearMin = 0xFFFFFFFFul;

unsigned long N3DS_HeapBallastBytes(void)
{
	return (unsigned long)s_simBallastHeap;
}

/* Heap bytes the game itself has handed out (simulation ballast excluded). */
unsigned long N3DS_HeapGameUsedBytes(void)
{
	const unsigned long used = (unsigned long)mallinfo().uordblks;

	return (used > s_simBallastHeap) ? (used - s_simBallastHeap) : 0ul;
}

/* Heap still available to malloc: the heap window minus what is handed out
 * (fragmentation not counted, so this is an upper bound). */
unsigned long N3DS_HeapFreeBytes(void)
{
	const unsigned long used = (unsigned long)mallinfo().uordblks;

	return (used < __ctru_heap_size) ? (__ctru_heap_size - used) : 0ul;
}

/* Cheap enough for every few frames: the malloc arena's extent (sbrk) and the
 * linear free space, folded into high/low-water marks. */
void N3DS_MemSample(void)
{
	const unsigned long arena = (unsigned long)((u32)sbrk(0) - __ctru_heap);
	const unsigned long lin = (unsigned long)linearSpaceFree();

	if (arena > s_arenaPeak)
	{
		s_arenaPeak = arena;
	}
	if (lin < s_linearMin)
	{
		s_linearMin = lin;
	}
}

/* One log line with every memory number worth keeping: grep "[MEM]" in the
 * log to get the real peak per level. Heap numbers exclude simulation ballast.
 * arenaPeak is the sampled malloc extent; hwm is newlib's own high-water mark
 * of total allocated space (mallinfo.usmblks, 0 if this libc does not keep it).
 * linMin is the lowest linear free seen at a sample point. */
void N3DS_MemLog(const char *tag)
{
	char line[320];
	const struct mallinfo mi = mallinfo();
	const unsigned long used = N3DS_HeapGameUsedBytes();
	const unsigned long lin = (unsigned long)linearSpaceFree();

	N3DS_MemSample();
	snprintf(line, sizeof(line),
	         "[MEM] %s heapUsed=%lu KiB heapFree=%lu KiB heapArenaPeak=%lu KiB hwm=%lu KiB | linFree=%lu KiB linMin=%lu KiB of %lu KiB | ballast heap=%lu lin=%lu KiB",
	         tag, used / 1024ul, N3DS_HeapFreeBytes() / 1024ul, (s_arenaPeak > s_simBallastHeap ? s_arenaPeak - s_simBallastHeap : 0ul) / 1024ul,
	         (unsigned long)mi.usmblks / 1024ul, lin / 1024ul, s_linearMin / 1024ul, (unsigned long)(__ctru_linear_heap_size / 1024),
	         (unsigned long)(s_simBallastHeap / 1024), (unsigned long)(s_simBallastLinear / 1024));
	N3DS_DebugLog(line);
}

/*
 * Old-3DS memory simulation: behave as if the process had been given a legacy
 * app region of `capMiB` (80 = the owner's other O3DS ports). Must run before
 * anything big is allocated (N3DS_GfxInit calls it first). Returns 1 if a cap
 * was applied (ballast taken), 0 if the real commit is already that small.
 */
int N3DS_HeapSimCap(unsigned capMiB)
{
	const u32 region = (u32)osGetMemRegionSize(MEMREGION_APPLICATION);
	const u32 staticBytes = (region > s_commitRemaining) ? (region - s_commitRemaining) : 0u;
	const u32 capBytes = capMiB * N3DS_MIB;
	u32 simRemaining, simLinear, simHeap, want;
	unsigned long freeLin;
	char line[256];

	if (capBytes <= staticBytes + N3DS_HEAP_MIN + N3DS_LINEAR_MIN)
	{
		N3DS_DebugLog("[O3DS-SIM] memory cap too small for this binary, ignored");
		return 0;
	}
	simRemaining = (capBytes - staticBytes) & ~0xFFFu;
	snprintf(line, sizeof(line), "[O3DS-SIM] app region %lu KiB, commit at boot %lu KiB => code+bss+stacks ~%lu KiB; emulating a %u MiB legacy region (%lu KiB left)",
	         (unsigned long)(region / 1024), (unsigned long)(s_commitRemaining / 1024), (unsigned long)(staticBytes / 1024), capMiB,
	         (unsigned long)(simRemaining / 1024));
	N3DS_DebugLog(line);
	if (simRemaining >= s_commitRemaining)
	{
		return 0;
	}
	simLinear = N3DS_LinearPlan(simRemaining);
	simHeap = N3DS_HeapPlan(simRemaining, simLinear);
	snprintf(line, sizeof(line), "[O3DS-SIM] legacy split would be: heap %lu KiB, linear %lu KiB (linear need ~%lu KiB%s)", (unsigned long)(simHeap / 1024),
	         (unsigned long)(simLinear / 1024), (unsigned long)(N3DS_LINEAR_REQUIRED / 1024),
	         (simLinear < N3DS_LINEAR_REQUIRED) ? " -- TOO SMALL, expect failures" : "");
	N3DS_DebugLog(line);

	/* Linear ballast first (nothing has used linear yet). */
	freeLin = (unsigned long)linearSpaceFree();
	if (freeLin > simLinear)
	{
		want = (u32)((freeLin - simLinear) & ~0xFFFul);
		while (want >= N3DS_HEAP_STEP)
		{
			if (linearAlloc(want) != NULL)
			{
				s_simBallastLinear = want;
				break;
			}
			want -= 256u * 1024u;
		}
	}
	/* Heap ballast: what the heap window has beyond the simulated heap, less
	 * what the game already holds. */
	{
		const unsigned long used = (unsigned long)mallinfo().uordblks;

		if ((__ctru_heap_size > simHeap) && (__ctru_heap_size - simHeap > used))
		{
			want = (u32)((__ctru_heap_size - simHeap - used) & ~0xFFFul);
			while (want >= N3DS_HEAP_STEP)
			{
				if (malloc(want) != NULL)
				{
					s_simBallastHeap = want;
					break;
				}
				want -= 256u * 1024u;
			}
		}
	}
	snprintf(line, sizeof(line), "[O3DS-SIM] ballast taken: heap %lu KiB, linear %lu KiB", (unsigned long)(s_simBallastHeap / 1024),
	         (unsigned long)(s_simBallastLinear / 1024));
	N3DS_DebugLog(line);
	N3DS_MemLog("simcap");
	return 1;
}
