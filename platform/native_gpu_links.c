#include <platform/native_gpu_links.h>
#include <platform/native_render_queue.h>

#include <macros.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define NATIVE_GPU_LINK_MAX_RANGES          64
#define NATIVE_GPU_LINK_FIRST_DYNAMIC_TOKEN 0x00f00000u

struct NativeGpuLinkRange
{
	uintptr_t hostStart;
	uintptr_t hostEnd;
	uint32_t tokenStart;
	uint32_t tokenEnd;
};

/*
 * Two tables: registration (Reset/Register, game thread, every ResetDB)
 * builds the PENDING one; token resolution during the walk reads the LIVE
 * one. Commit promotes pending to live -- a memcmp no-op every frame the
 * ranges did not move (steady state within a level), and on real change
 * (level load resizes primMem) it fences the render queue first, so the
 * worker can never resolve against a half-rewritten table. Before the render
 * thread existed the split was free; with it, it is the correctness.
 */
global_variable struct NativeGpuLinkRange s_nativeGpuLinkRanges[NATIVE_GPU_LINK_MAX_RANGES];
global_variable int s_nativeGpuLinkRangeCount;
global_variable struct NativeGpuLinkRange s_nativeGpuLinkPending[NATIVE_GPU_LINK_MAX_RANGES];
global_variable int s_nativeGpuLinkPendingCount;
global_variable uint32_t s_nativeGpuLinkNextToken = NATIVE_GPU_LINK_FIRST_DYNAMIC_TOKEN;
/*
 * Last-hit hints for the two range scans. Every addPrim on the game thread
 * resolves a host pointer to a token, and the OT walk resolves each node's
 * link token back and then validates the host pointer -- three scans of the
 * live table per primitive, and nearly every one lands in the same range as
 * the one before (the frame's primMem, or its OT). The hint is checked first
 * and the scan only runs on a miss. Correctness does not depend on the hint:
 * it is only an index into the live table, re-validated against that range's
 * bounds on every use, and only trusted when the live ranges are pairwise
 * disjoint (checked at Commit) -- otherwise the scan's first-match answer
 * could differ from the hinted range. The game thread and the render worker
 * may both write a hint; a torn race costs one scan, never a wrong answer
 * (an aligned int store is atomic, and the table itself only changes under
 * Commit's render-queue fence).
 */
global_variable int s_nativeGpuLinkHostHint;
global_variable int s_nativeGpuLinkTokenHint;
global_variable int s_nativeGpuLinkHintsUsable;

static int NativeGpuLinks_AlignTokenSize(size_t size, uint32_t *tokenSizeOut)
{
	size_t alignedSize;

	if ((size == 0) || (size > NATIVE_GPU_LINK_FIRST_DYNAMIC_TOKEN) || (size > (SIZE_MAX - 3u)))
	{
		return 0;
	}

	alignedSize = (size + 3u) & ~(size_t)3u;
	if (alignedSize > NATIVE_GPU_LINK_FIRST_DYNAMIC_TOKEN)
	{
		return 0;
	}

	*tokenSizeOut = (uint32_t)alignedSize;
	return 1;
}

void NativeGpuLinks_Reset(void)
{
	s_nativeGpuLinkPendingCount = 0;
	s_nativeGpuLinkNextToken = NATIVE_GPU_LINK_FIRST_DYNAMIC_TOKEN;
}

void NativeGpuLinks_Commit(void)
{
	size_t liveBytes = (size_t)s_nativeGpuLinkPendingCount * sizeof(s_nativeGpuLinkPending[0]);

	if ((s_nativeGpuLinkPendingCount == s_nativeGpuLinkRangeCount) && (memcmp(s_nativeGpuLinkPending, s_nativeGpuLinkRanges, liveBytes) == 0))
	{
		return;
	}

	NativeRenderQueue_Fence();
	memcpy(s_nativeGpuLinkRanges, s_nativeGpuLinkPending, liveBytes);
	s_nativeGpuLinkRangeCount = s_nativeGpuLinkPendingCount;

	/* Hints (see their declaration) only when no two ranges overlap, in host
	 * or token space: then at most one range contains any pointer or token,
	 * and the hinted range IS the scan's answer whenever it matches. Token
	 * ranges are carved downward without overlap by construction; host
	 * ranges are separate allocations, but checked rather than assumed. */
	s_nativeGpuLinkHostHint = 0;
	s_nativeGpuLinkTokenHint = 0;
	s_nativeGpuLinkHintsUsable = 1;
	for (int i = 0; i < s_nativeGpuLinkRangeCount; i++)
	{
		for (int j = i + 1; j < s_nativeGpuLinkRangeCount; j++)
		{
			const struct NativeGpuLinkRange *a = &s_nativeGpuLinkRanges[i];
			const struct NativeGpuLinkRange *b = &s_nativeGpuLinkRanges[j];

			if (((a->hostStart < b->hostEnd) && (b->hostStart < a->hostEnd)) || ((a->tokenStart < b->tokenEnd) && (b->tokenStart < a->tokenEnd)))
			{
				s_nativeGpuLinkHintsUsable = 0;
			}
		}
	}
}

int NativeGpuLinks_IsTerminator(uint32_t token)
{
	return (token & 0x00ffffffu) == NATIVE_GPU_LINK_TERMINATOR;
}

int NativeGpuLinks_RegisterRange(const void *hostStart, size_t size, uint32_t *tokenStartOut)
{
	uint32_t tokenSize;
	uintptr_t start;
	uintptr_t end;

	if ((hostStart == NULL) || (size == 0))
	{
		return 0;
	}

	if (s_nativeGpuLinkPendingCount >= NATIVE_GPU_LINK_MAX_RANGES)
	{
		return 0;
	}

	if (!NativeGpuLinks_AlignTokenSize(size, &tokenSize) || (tokenSize > s_nativeGpuLinkNextToken))
	{
		return 0;
	}

	start = (uintptr_t)hostStart;
	end = start + (uintptr_t)size;
	if (end < start)
	{
		return 0;
	}

	s_nativeGpuLinkNextToken -= tokenSize;

	struct NativeGpuLinkRange *range = &s_nativeGpuLinkPending[s_nativeGpuLinkPendingCount++];
	range->hostStart = start;
	range->hostEnd = end;
	range->tokenStart = s_nativeGpuLinkNextToken;
	range->tokenEnd = s_nativeGpuLinkNextToken + tokenSize;

	if (tokenStartOut != NULL)
	{
		*tokenStartOut = range->tokenStart;
	}

	return 1;
}

void NativeGpuLinks_RegisterRangeChecked(const char *label, const void *hostStart, size_t size)
{
	if (NativeGpuLinks_RegisterRange(hostStart, size, NULL))
	{
		return;
	}

	fprintf(stderr, "[CTR 3D] GPU link bridge failed to register %s range start=%p size=%zu\n", label, hostStart, size);
	abort();
}

static const struct NativeGpuLinkRange *NativeGpuLinks_FindHostRange(uintptr_t hostPtr)
{
	const int hint = s_nativeGpuLinkHostHint;

	if (s_nativeGpuLinkHintsUsable && (hint < s_nativeGpuLinkRangeCount))
	{
		const struct NativeGpuLinkRange *range = &s_nativeGpuLinkRanges[hint];
		if ((hostPtr >= range->hostStart) && (hostPtr < range->hostEnd))
		{
			return range;
		}
	}

	for (int i = 0; i < s_nativeGpuLinkRangeCount; i++)
	{
		const struct NativeGpuLinkRange *range = &s_nativeGpuLinkRanges[i];
		if ((hostPtr >= range->hostStart) && (hostPtr < range->hostEnd))
		{
			s_nativeGpuLinkHostHint = i;
			return range;
		}
	}

	return NULL;
}

static const struct NativeGpuLinkRange *NativeGpuLinks_FindTokenRange(uint32_t token)
{
	const int hint = s_nativeGpuLinkTokenHint;

	token &= 0x00ffffffu;

	if (s_nativeGpuLinkHintsUsable && (hint < s_nativeGpuLinkRangeCount))
	{
		const struct NativeGpuLinkRange *range = &s_nativeGpuLinkRanges[hint];
		if ((token >= range->tokenStart) && (token < range->tokenEnd))
		{
			return range;
		}
	}

	for (int i = 0; i < s_nativeGpuLinkRangeCount; i++)
	{
		const struct NativeGpuLinkRange *range = &s_nativeGpuLinkRanges[i];
		if ((token >= range->tokenStart) && (token < range->tokenEnd))
		{
			s_nativeGpuLinkTokenHint = i;
			return range;
		}
	}

	return NULL;
}

uint32_t NativeGpuLinks_FromHostPointer(const void *hostPtr)
{
	uintptr_t host = (uintptr_t)hostPtr;
	const struct NativeGpuLinkRange *range = NativeGpuLinks_FindHostRange(host);

	if (range != NULL)
	{
		return range->tokenStart + (uint32_t)(host - range->hostStart);
	}

	fprintf(stderr, "[CTR 3D] GPU link bridge has no token for host pointer %p\n", hostPtr);
	abort();
}

void *NativeGpuLinks_ToHostPointer(uint32_t token)
{
	const struct NativeGpuLinkRange *range;

	token &= 0x00ffffffu;
	if (NativeGpuLinks_IsTerminator(token))
	{
		return NULL;
	}

	range = NativeGpuLinks_FindTokenRange(token);
	if (range != NULL)
	{
		return (void *)(range->hostStart + (uintptr_t)(token - range->tokenStart));
	}

	return NULL;
}

int NativeGpuLinks_IsRegisteredHostPointer(const void *hostPtr)
{
	return NativeGpuLinks_FindHostRange((uintptr_t)hostPtr) != NULL;
}

int NativeGpuLinks_IsRegisteredHostRange(const void *hostPtr, size_t size)
{
	uintptr_t start;
	uintptr_t end;

	if ((hostPtr == NULL) || (size == 0))
	{
		return 0;
	}

	start = (uintptr_t)hostPtr;
	end = start + (uintptr_t)size;
	if (end < start)
	{
		return 0;
	}

	for (int i = 0; i < s_nativeGpuLinkRangeCount; i++)
	{
		const struct NativeGpuLinkRange *range = &s_nativeGpuLinkRanges[i];
		if ((start >= range->hostStart) && (end <= range->hostEnd))
		{
			return 1;
		}
	}

	return 0;
}
