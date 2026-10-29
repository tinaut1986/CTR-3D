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
 */
#include <3ds.h>
#include <stdio.h>

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

static u32 s_commitRemaining = 0;

void __system_allocateHeaps(void)
{
	const u32 pageMask = 0xFFF;
	const u32 heapMax = (OS_HEAP_AREA_END - OS_HEAP_AREA_BEGIN) - (4u * N3DS_MIB);
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

	/* Linear first. */
	linear = N3DS_LINEAR_WANT;
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
		 * (Old 3DS): even split, the game logs what does not fit. */
		linear = (remaining / 2) & ~pageMask;
	}
	linear &= ~pageMask;

	/* Heap: everything else, within its address window. */
	heap = remaining - linear;
	if (heap > heapMax)
	{
		heap = heapMax;
	}
	heap &= ~pageMask;
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
