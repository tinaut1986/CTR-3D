/*
 * Direct-emit fast path (retained-geometry campaign, transitional form). For an OT node whose payload is
 * nothing but opaque poly prims, emits packet -> merge bin directly, skipping
 * the GrVertex/split path. Byte-parity with the normal path is the design
 * invariant (see DirectEmit_WalkNode). A node with any draw-env prim,
 * non-poly prim, semi-trans poly, TF_16_BIT textured poly (framebuffer
 * feedback), or malformed tiling stays on the normal path. PC: count-only,
 * TryNode always returns 0.
 *
 * This file is part of the main.c unity TU and is included AFTER
 * native_gpu.c and platform/n3ds/native_renderer_3ds.c so it can see their
 * file-scope state (s_gpu, activeDrawEnv, merge bins, the decode-cache
 * resolve).
 */

#ifndef CTR_NATIVE_DIRECT_EMIT
#define CTR_NATIVE_DIRECT_EMIT 0
#endif

#if defined(CTR_NATIVE) && CTR_NATIVE_DIRECT_EMIT

/*
 * Runtime A/B toggle: dumpreq 'directemit'. Read live per node rather than
 * latched at BeginScene: unlike 'drawmerge' (whose flip changes barrier
 * semantics), both legs here produce byte-identical bins, so a mid-frame
 * flip just splits one frame's content between two equivalent paths.
 *
 * PC defaults 0: with the gate promoted to a default-ON build option,
 * a PC default of 1 would put the count-only
 * scan and a [DEMIT] line in every PC user's run for no benefit -- the win
 * is 3DS-only. Flip to 1 in source when a PC replay needs the eligibility
 * counts (edit-and-build diagnostic, same convention as
 * g_dbg_disableSubPage).
 */
#ifdef __3DS__
int g_dbg_directEmit = 1;
#else
int g_dbg_directEmit = 0;
#endif

/* Node-skip reasons, one per node (first disqualifier wins). */
enum
{
	DEMIT_OK = 0,
	DEMIT_SKIP_ENV,
	DEMIT_SKIP_NONPOLY,
	DEMIT_SKIP_BLEND,
	DEMIT_SKIP_FMT16,
	DEMIT_SKIP_MALFORMED,
	DEMIT_SKIP_COUNT
};

typedef struct
{
	/* per-frame, reset by DirectEmit_FoldFrame */
	u32 frameNodes;
	u32 frameElig;
	u32 framePrims;
	u32 frameTris;
	u32 frameNullPrims;
	u32 frameEmitTris;
	u32 frameSkip[DEMIT_SKIP_COUNT];

	/* window accumulators, printed every 64 content frames */
	u32 winFrames;
	u32 winNodes;
	u32 winElig;
	u32 winPrims;
	u32 winTris;
	u32 winNullPrims;
	u32 winEmitTris;
	u32 winSkip[DEMIT_SKIP_COUNT];
} DirectEmitStats;

global_variable DirectEmitStats s_demit;

#define DEMIT_WINDOW_FRAMES 64

/* DirectEmit_WalkNode's `emit` argument. */
enum
{
	DEMIT_PASS_DRY = 0,           /* validate + count, no side effects */
	DEMIT_PASS_EMIT = 1,          /* emit only: the dry pass already ran */
	DEMIT_PASS_COUNT_AND_EMIT = 2 /* single-prim node: validate, count, emit */
};

/*
 * One walker, two passes: emit=0 validates the whole node and counts content
 * (pure -- no state touched), emit=1 re-walks and emits. A node holding ONE
 * prim takes a single pass instead (DEMIT_PASS_COUNT_AND_EMIT): every
 * disqualifier is tested before that prim's first side effect, so there is
 * nothing for a dry pass to protect -- see NativeDirectEmit_TryNode. Sharing the walk
 * keeps the layout tables single-copy; the dry pass guarantees the emit pass
 * cannot encounter a disqualifier after side effects have started.
 * Word layouts mirror ProcessFlatPoly/ProcessGouraudPoly exactly; a node
 * whose prim lengths do not tile tagLength is rejected, same guard as the
 * [OTPROF] shape walker.
 */
/*
 * knownCode: -1 for the generic walker; otherwise the caller guarantees the
 * node is exactly one prim with this command byte, and the constant lets the
 * compiler fold the shape decode and every branch that depends on it into a
 * walker specialised for that one prim type (DirectEmit_WalkNodeG4). Same
 * source, so the same bytes by construction.
 */
#ifdef __3DS__
/* What the last emitted prim's depth resolve returned (set in the emit below,
 * read by NativeDirectEmit_G4Run to seed the flag run's cached depth). */
global_variable int s_g4LastResolved;
global_variable u16 s_g4LastStamp;
global_variable u16 s_g4LastPerV[4];
#endif

force_inline int DirectEmit_WalkNodeImpl(const u32 *words, u32 len, int emit, int knownCode)
{
	u32 w = 0;

#ifdef __3DS__
	float ofsX = 0.0f, ofsY = 0.0f;

	if (emit)
	{
		/* Once per node: env prims are excluded, so the offset cannot
		 * change mid-node. Float on purpose -- MakeVertexTriangle/Quad add
		 * this exact float to the s16 packet coordinate, and byte-parity
		 * beats saving two VFP converts per vertex. */
		DrawEnvOffset(&ofsX, &ofsY);
	}
#else
	(void)emit;
#endif

	while (w < len)
	{
		const u32 cmd = words[w];
		const u32 code = (knownCode >= 0) ? (u32)knownCode : (cmd >> 24);

		if ((code < 0x20u) || (code > 0x3Fu))
		{
			return ((code >= 0xE0u) && (code <= 0xE7u)) ? DEMIT_SKIP_ENV : DEMIT_SKIP_NONPOLY;
		}
		if (code & 0x02u)
		{
			return DEMIT_SKIP_BLEND;
		}

		{
			const int gouraud = (code & 0x10u) != 0;
			const int quadPrim = (code & 0x08u) != 0;
			const int textured = (code & 0x04u) != 0;
			const int numVerts = quadPrim ? 4 : 3;
			const u32 primLen = 1u + (u32)numVerts * (textured ? 2u : 1u) + (gouraud ? (u32)(numVerts - 1) : 0u);
			u32 xy[4], uvw[4], rgb[4];
			u32 c = w + 1;
			int v;

			if (w + primLen > len)
			{
				return DEMIT_SKIP_MALFORMED;
			}

			uvw[0] = uvw[1] = uvw[2] = uvw[3] = 0;
			rgb[0] = cmd & 0x00FFFFFFu;
			for (v = 0; v < numVerts; v++)
			{
				if (v > 0)
				{
					rgb[v] = gouraud ? (words[c++] & 0x00FFFFFFu) : rgb[0];
				}
				xy[v] = words[c++];
				if (textured)
				{
					uvw[v] = words[c++];
				}
			}

			/* tpage rides the second vertex's UV word, clut the first --
			 * the POLY_FT/GT struct layout. */
			if (textured && (GetTPageFormat((int)(s16)(uvw[1] >> 16)) == TF_16_BIT))
			{
				return DEMIT_SKIP_FMT16;
			}

			/* Counting runs on the dry pass (0) and on the single-pass form
			 * (DEMIT_PASS_COUNT_AND_EMIT), never on a second, emit-only pass. */
			if (emit != DEMIT_PASS_EMIT)
			{
				s_demit.framePrims++;
				if (!gouraud && textured && !quadPrim && (xy[0] == 0xFFFFFFFFu) && (xy[1] == 0xFFFFFFFFu) && (xy[2] == 0xFFFFFFFFu))
				{
					s_demit.frameNullPrims++;
				}
				else
				{
					s_demit.frameTris += quadPrim ? 2u : 1u;
				}
			}
#ifdef __3DS__
			if (emit)
			{
				const s16 rawTPage = (s16)(uvw[1] >> 16);
				const int isNullFT3 =
				    (!gouraud && textured && !quadPrim && (xy[0] == 0xFFFFFFFFu) && (xy[1] == 0xFFFFFFFFu) && (xy[2] == 0xFFFFFFFFu));
				u16 ordinal;
				s16 z;
				s16 zPerVert[4];
				/* Parallax channel twin:
				 * same indices, same validity flag -- the resolver fills both
				 * or neither. */
				s16 wPerVert[4];
				int zPerVertValid;

				/* Side effect order matches ProcessFlat/GouraudPoly: the
				 * tpage write happens before anything else, null or not. */
				if (textured)
				{
					activeDrawEnv.tpage = rawTPage;
				}

				/* One stamp per prim, unconditionally -- the normal path's
				 * StampEmitOrdinal advances even for the null-FT3 DR_TPAGE
				 * hack (it stamps zero vertices, then increments). Same
				 * shared stamp source as the parse path, so the ordinal
				 * widening scheme (dumpreq 'otz') covers direct emit too. */
				ordinal = NativeGpu_NextDepthStamp();
				s_gpu.emitOrdinal++;
				/* Depth-compose fix (native_gpu.c's NativeGpu_RecordContentAdvance
				 * comment): this fast path advances the emit ordinal exactly
				 * like the normal parse path's two NativeGpu_StampEmitOrdinal
				 * call sites, so it must feed minContentBucket the same way --
				 * otherwise every direct-emitted node (most of a heavy track's
				 * opaque content) would be invisible to the depth-compose
				 * floor, and the fix would only work when direct emit happens
				 * to be off. */
				NativeGpu_RecordContentAdvance();
				z = (s16)((int)ordinal - 32768);
				/* Per-vertex depth. Stays invalid -- and every vertex
				 * keeps the flat z above -- unless the resolver below fills it,
				 * so the feature-off and fallback paths are byte-unchanged. */
				zPerVertValid = 0;
				s_g4LastResolved = 0;

#if defined(CTR_NATIVE) && CTR_NATIVE_GTE_CAPTURE
				/* Depth model. This path
				 * writes its own z and handles most of a heavy track's opaque
				 * content, so it MUST resolve depth exactly as the parse path
				 * does -- if only one of the two moves onto the true-depth
				 * axis, the halves of the same frame end up on different
				 * scales. Returns 0 with the feature off, leaving the ordinal
				 * above untouched. */
				{
					u16 depthStamp;
					u16 depthPerVert[4];
					u16 depthPerVertPar[4];

#if defined(CTR_INTERNAL)
					const u64 profResolveStart = (s_parseProfSampleNode || s_parseProfG4Sample) ? PARSEPROF_TICK() : 0;
					const int resolved = PacketDepth_ResolvePacked(xy, numVerts, &depthStamp, depthPerVert, depthPerVertPar);

					if (s_parseProfG4Sample)
					{
						s_parseProf.g4ResolveTicks += PARSEPROF_TICK() - profResolveStart;
					}
					else if (s_parseProfSampleNode)
					{
						s_parseProf.demitResolveTicks += PARSEPROF_TICK() - profResolveStart;
					}
					if (resolved)
#else
					if (PacketDepth_ResolvePacked(xy, numVerts, &depthStamp, depthPerVert, depthPerVertPar))
#endif
					{
						int v;

						s_g4LastResolved = 1;
						s_g4LastStamp = depthStamp;
						s_g4LastPerV[0] = depthPerVert[0];
						s_g4LastPerV[1] = depthPerVert[1];
						s_g4LastPerV[2] = depthPerVert[2];
						s_g4LastPerV[3] = depthPerVert[3];
						z = (s16)((int)depthStamp - 32768);
						/* Per-vertex depth: keep each corner's own depth so
						 * the GPU interpolates across the polygon, instead of
						 * flattening the whole primitive onto its FARTHEST
						 * corner (retail's OT-bucket convention, which loses to
						 * the resident track's real interpolated depth wherever
						 * the two meet -- the road drawing over Crash). Indexed
						 * by SOURCE vertex below via idx[], so triangulation
						 * order cannot pair a vertex with a stranger's depth. */
						for (v = 0; v < numVerts && v < 4; v++)
						{
							zPerVert[v] = (s16)((int)depthPerVert[v] - 32768);
							wPerVert[v] = (s16)((int)depthPerVertPar[v] - 32768);
						}
						zPerVertValid = 1;
					}
				}
#endif

				if (!isNullFT3)
				{
					const int noShade = (!gouraud && (code & 0x01u));
					const s16 texPage = textured ? GetTPageBase((int)rawTPage) : 0;
					const s16 clut = textured ? (s16)(uvw[0] >> 16) : 0;
					const TexFormat fmt = textured ? GetTPageFormat((int)rawTPage) : TF_4_BIT;
					const int numTris = quadPrim ? 2 : 1;
					/* Each SOURCE vertex converted once: a quad's two
					 * triangles share vertices 1 and 2, which the per-triangle
					 * form converted twice (6 conversions for 4 vertices). Every
					 * field but u/v depends only on the source vertex and this
					 * prim's node flags, so the triangles copy these and only
					 * u/v are written per triangle (they depend on the
					 * triangle's own texture resolve). Same bytes as before. */
					N3DS_Vertex srcVert[4];
					int t;

					for (v = 0; v < numVerts; v++)
					{
						const u32 xyWord = xy[v];
						const u32 col = rgb[v];
						s16 dx = (s16)((float)(s16)(xyWord & 0xFFFFu) + ofsX);
						const s16 dz = zPerVertValid ? zPerVert[v] : z;
						N3DS_Vertex *sv = &srcVert[v];

						/* Widescreen UI relocation -- same rule as N3DS_ConvertTriangleVerts's:
						 * UI-band stamps (biased z >= 32704 = raw 0xFFC0) get the X ramp
						 * (NativeWide_UiShiftX). zPerVertValid doubles as the packet-depth-live
						 * gate: an ordinal z cannot reach the band. No-op with wide off, so
						 * PC/[DEMIT] goldens are untouched. */
						if (zPerVertValid && (dz >= 32704))
						{
							extern int NativeWide_UiShiftX(int x);

							dx = (s16)NativeWide_UiShiftX((int)dx);
						}
						/* s46: the horizon gradient is screen-space geometry hardcoded
						 * to the retail 512-wide rect; stretch it to the wide ortho
						 * window (identity with wide off). Glow slice only -- see
						 * NativeWide_GlowStretchX for why the dome must not take this.
						 * Mutually exclusive with the UI ramp above by depth: glow is
						 * far-pinned. */
						if (s_glowNodeActive)
						{
							extern int NativeWide_GlowStretchX(int x);

							dx = (s16)NativeWide_GlowStretchX((int)dx);
						}
						/* Menu backdrop: same ramp as the window border these
						 * strips sit inside, which is UI and already takes it --
						 * see MakeVertexQuad's copy. Mutually exclusive with the
						 * UI branch above by depth (the backdrop is midband). */
						if (s_menubdNodeActive)
						{
							extern int NativeWide_UiShiftX(int x);

							dx = (s16)NativeWide_UiShiftX((int)dx);
						}
						sv->x = dx;
						sv->y = (s16)((float)(s16)(xyWord >> 16) + ofsY);
						sv->z = dz;
						/* Parallax channel: the resolver's second
						 * output where it exists, otherwise the same value
						 * as z -- an ordinal-stamped or unresolved prim has
						 * no separate stereo depth to carry. */
						sv->w = zPerVertValid ? wPerVert[v] : dz;
						/* Untextured: the white cell, per vertex constant.
						 * Textured: written per triangle below. */
						sv->u = (s16)(((N3DS_WHITE_CELL_X * N3DS_DECODE_TEXTURE_DIM + (N3DS_DECODE_TEXTURE_DIM / 2)) << 1) + 1);
						sv->v = (s16)(2047 - ((N3DS_WHITE_CELL_Y * N3DS_DECODE_TEXTURE_DIM + (N3DS_DECODE_TEXTURE_DIM / 2)) << 1));
						if (noShade)
						{
							sv->r = 128;
							sv->g = 128;
							sv->b = 128;
						}
						else
						{
							sv->r = (u8)(col & 0xFFu);
							sv->g = (u8)((col >> 8) & 0xFFu);
							sv->b = (u8)((col >> 16) & 0xFFu);
						}
						sv->_pad = 0;
					}

					/* Packet-index triangles (0,1,2) and (1,2,3): what
					 * MakeVertexQuad's x0,x1,x3,x2 argument order and
					 * TriangulateQuad's element swaps net out to. */
					for (t = 0; t < numTris; t++)
					{
						int bin, atlasIdx = 0, pbU = 0, pbV = 0, oU = 0, oV = 0;
						N3DS_Vertex *dst;
						int k;

						if (textured)
						{
							/* Resolve runs per frame per triangle exactly as
							 * the normal path's DrawTriangles loop does --
							 * decode-on-miss, LRU touch, fb-mirror refresh,
							 * sub-page/full-page routing all preserved. Only
							 * .u/.v of these are read (see
							 * N3DS_ResolveTriangleTexture). */
							GrVertex tv[3];

							for (k = 0; k < 3; k++)
							{
								tv[k].u = (u8)(uvw[t + k] & 0xFFu);
								tv[k].v = (u8)((uvw[t + k] >> 8) & 0xFFu);
							}
							N3DS_ResolveTriangleTexture(&tv[0], &tv[1], &tv[2], texPage, clut, fmt, N3DS_TEXVIEW_OPAQUE, &atlasIdx, &pbU, &pbV, &oU,
							                            &oV);
							bin = atlasIdx;
						}
						else
						{
							bin = N3DS_MERGE_UNTEX_BIN;
						}

						/* Sky-first: the skybox (tagged by s_skyNodeActive during the walk,
						 * native_gpu.c) accumulates into its own buffer so N3DS_DrawSkyPass can
						 * draw it before the track pass -- otherwise, binned as ordinary opaque,
						 * it flushes after the track and paints over the depth-less waterfall. */
						if (g_dbg_skyFirst && s_skyNodeActive)
						{
							if ((s_skyVertUsed + 3) > N3DS_SKY_VERT_CAP)
							{
								continue;
							}
							dst = &s_skyVerts[s_skyVertUsed];
						}
						else
						{
							if ((s_mergeBinUsed[bin] + 3) > N3DS_MERGE_BIN_CAP_VERTS)
							{
								N3DS_MergeSpillBin(bin);
							}
							dst = &s_mergeBinVerts[bin][s_mergeBinUsed[bin]];
						}

						/* Triangle t is source vertices t, t+1, t+2. */
						dst[0] = srcVert[t];
						dst[1] = srcVert[t + 1];
						dst[2] = srcVert[t + 2];
						if (textured)
						{
							for (k = 0; k < 3; k++)
							{
								const int srcU = (int)(uvw[t + k] & 0xFFu);
								const int srcV = (int)((uvw[t + k] >> 8) & 0xFFu);

								dst[k].u = (s16)(((pbU + (srcU - oU)) << 1) + 1);
								dst[k].v = (s16)(2047 - ((pbV + (srcV - oV)) << 1));
							}
						}
						/* Advance whichever buffer this tri went to. */
						if (g_dbg_skyFirst && s_skyNodeActive)
						{
							s_skyVertUsed += 3;
						}
						else
						{
							s_mergeBinUsed[bin] += 3;
						}
						s_demit.frameEmitTris++;
					}
				}
			}
#endif /* __3DS__ */

			w += primLen;
		}
	}

#ifdef __3DS__
	if (emit)
	{
		/* Every fast-path prim is non-feedback, and AddSplit's non-feedback
		 * branch clears this flag per prim -- keep the feedback-run state
		 * machine seeing the same sequence it would have. */
		s_gpu.framebufferFeedbackRunActive = false;
	}
#endif

	return DEMIT_OK;
}

internal int DirectEmit_WalkNode(const u32 *words, u32 len, int emit)
{
	return DirectEmit_WalkNodeImpl(words, len, emit, -1);
}

/* Single-pass walker for a one-prim POLY_G4 node (code 0x38: gouraud quad,
 * untextured, opaque, shaded) -- the shape of every menu/loading flag quad
 * (RaceFlag_DrawSelf). */
#define DEMIT_CODE_POLY_G4 0x38u
#define DEMIT_LEN_POLY_G4  8u

internal int DirectEmit_WalkNodeG4(const u32 *words)
{
	return DirectEmit_WalkNodeImpl(words, DEMIT_LEN_POLY_G4, DEMIT_PASS_COUNT_AND_EMIT, (int)DEMIT_CODE_POLY_G4);
}

/*
 * Whether the fast path may emit the node the walk is on (per node: the UI
 * band latch and the hud-shine tag change during a walk). PC: never.
 */
internal int DirectEmit_NodeActive(void)
{
	int active;

#ifdef __3DS__
	active = s_mergeActive && (s_gpu.overrideTexture == 0) && !s_gpu.drawPrimMode && !g_dbg_texturelessMode && !g_dbg_forceSingleTexture &&
	         (g_dbg_atlasView == 0);
#ifdef CTR_INTERNAL
	/* An armed triangle dump observes the normal path's DrawTriangles loop;
	 * fall back for that frame so its record set stays complete. */
	active = active && (s_triDumpFile == NULL);
#endif
#if defined(CTR_NATIVE_VERTEX_DESNAP) && CTR_NATIVE_VERTEX_DESNAP
	/* Desnap carries sub-pixel fractions in GrVertex padding; the fast path
	 * has no GrVertex to carry them in. */
	active = 0;
#endif
#if defined(CTR_NATIVE) && CTR_NATIVE_GTE_CAPTURE
	/*
	 * Stand aside for 2D/UI nodes so UI has exactly one producer. The ordered UI
	 * list is only correct if appends arrive in OT-walk order, and the two
	 * producing paths do not interleave: direct emit writes vertices DURING the
	 * parse, while the packet path's prims replay later from splits (DrawOTag
	 * parses, then DrawAllSplits runs) -- so a list fed by both is ordered
	 * [all direct-emit UI][all packet UI], not walk order. Declining the node
	 * routes it through the packet path instead, which preserves walk order
	 * within itself. Unconditional (not gated on g_dbg_uiOrder): the same
	 * two-producer defect breaks UI either way. s_gteCaptureSuppress is the same
	 * predicate PacketDepth_ResolveXY uses for the UI band, so the two paths
	 * cannot disagree about what counts as UI. The sky is not caught here
	 * because the suppress flag latches only once the walk enters the UI
	 * pushbuffer address band, and the sky walks before it.
	 */
	/* The HUD shine span joins the predicate here exactly as it does in the
	 * resolver, so the two paths keep agreeing about what counts as UI. */
	if (GTECapture_LookupSuppressed() || (g_dbg_hudShine && s_hudshineNodeActive))
	{
		active = 0;
	}
#endif
#else
	/* PC: the renderer consumes splits, not bins -- measure, never emit. */
	active = 0;
#endif

	return active;
}

/*
 * Called from ParsePrimitivesLinkedList per OT node (after the tagLength
 * validity checks, before the parse loop). Returns 1 when the node was fully
 * emitted here and the parse loop must be skipped; 0 means parse normally
 * (ineligible node, fast path inactive, or PC count-only build).
 */
internal int NativeDirectEmit_TryNode(const u8 *packet, u32 tagLength)
{
	const u32 *words = (const u32 *)packet + P_LEN;
	int active;
	int scan;

	/* OFF leg of the A/B (and the PC default) stays truly zero-cost: no
	 * scan, no counters. */
	if (!g_dbg_directEmit)
	{
		return 0;
	}

	active = DirectEmit_NodeActive();

#ifdef __3DS__
	/*
	 * Single-prim node (the menu flag's ~1000 quads are one POLY_G4 each, and
	 * most race nodes hold one prim): one pass validates, counts and emits.
	 * The dry pass exists so a LATER prim's disqualifier cannot surface after
	 * an earlier prim's side effects; with one prim every test runs before
	 * its first side effect, and a rejected node returns having touched
	 * nothing, exactly as the dry pass would. Counters and bins come out
	 * identical; the saving is the second decode of the packet.
	 */
	if (active)
	{
		const u32 code = words[0] >> 24;

		if ((code >= 0x20u) && (code <= 0x3Fu))
		{
			const int numVerts = (code & 0x08u) ? 4 : 3;
			const u32 primLen = 1u + (u32)numVerts * ((code & 0x04u) ? 2u : 1u) + ((code & 0x10u) ? (u32)(numVerts - 1) : 0u);

			if (primLen == tagLength)
			{
				s_demit.frameNodes++;
				if (g_dbg_perfTriScopes)
				{
					NativePerf_BeginScope(NATIVE_PERF_BUCKET_DIRECT_EMIT);
				}
				scan = ((code == DEMIT_CODE_POLY_G4) && (tagLength == DEMIT_LEN_POLY_G4)) ? DirectEmit_WalkNodeG4(words)
				                                                                          : DirectEmit_WalkNode(words, tagLength, DEMIT_PASS_COUNT_AND_EMIT);
				if (g_dbg_perfTriScopes)
				{
					NativePerf_EndScope(NATIVE_PERF_BUCKET_DIRECT_EMIT);
				}
				if (scan != DEMIT_OK)
				{
					s_demit.frameSkip[scan]++;
					return 0;
				}
				s_demit.frameElig++;
				return 1;
			}
		}
	}
#endif

#if defined(CTR_INTERNAL) && defined(__3DS__)
	{
		const u64 profScanStart = s_parseProfSampleNode ? PARSEPROF_TICK() : 0;

		scan = DirectEmit_WalkNode(words, tagLength, DEMIT_PASS_DRY);
		if (s_parseProfSampleNode)
		{
			s_parseProf.demitScanTicks += PARSEPROF_TICK() - profScanStart;
		}
	}
#else
	scan = DirectEmit_WalkNode(words, tagLength, DEMIT_PASS_DRY);
#endif
	s_demit.frameNodes++;
	if (scan != DEMIT_OK)
	{
		s_demit.frameSkip[scan]++;
		return 0;
	}
	s_demit.frameElig++;

	if (!active)
	{
		return 0;
	}

#ifdef __3DS__
	/* One scope per handled node (~1-1.5k/frame in a race) under the same
	 * runtime flag as the other per-call scopes: splits the moved emit work
	 * out of draw_otag_parse_ms (the finding this bucket exists for). */
	if (g_dbg_perfTriScopes)
	{
		NativePerf_BeginScope(NATIVE_PERF_BUCKET_DIRECT_EMIT);
		DirectEmit_WalkNode(words, tagLength, DEMIT_PASS_EMIT);
		NativePerf_EndScope(NATIVE_PERF_BUCKET_DIRECT_EMIT);
		return 1;
	}
#endif

	DirectEmit_WalkNode(words, tagLength, DEMIT_PASS_EMIT);
	return 1;
}

#ifdef __3DS__
/*
 * Flag-quad fast path inside the G4 run (dumpreq 'g4fast' A/Bs it, 'g4check'
 * verifies it on the console).
 *
 * Every quad of the menu/loading flag is a POLY_G4 of the same exempt range, in
 * one OT bucket, one UI-latch state, one draw env: the depth resolve gives all
 * of them the SAME occlusion stamp (the sheet's UI stamp, or the bucket-derived
 * one), flat across the quad, and only the parallax channel differs, read from
 * the quad's own table slot. So the first node of a run (emitted by the generic
 * walker) seeds a context, and the rest skip the resolve, the float vertex
 * conversion and the per-prim decode, producing the same N3DS_Vertex bytes. A
 * quad whose state differs from the seed goes through the generic walker.
 */
int g_dbg_g4Fast = 1;
int g_dbg_g4Check = 0;

typedef struct G4FastCtx
{
	int valid;
	int suppressed;
	int slotIdx;
	int ofsX, ofsY;
	int uiShift;
	u16 stamp;
	s16 z;
	s16 u, v;
} G4FastCtx;

global_variable u32 s_g4CheckQuads;
global_variable u32 s_g4CheckBad;

internal void DirectEmit_G4FastSeed(G4FastCtx *c)
{
	float ofsX = 0.0f, ofsY = 0.0f;

	c->valid = 0;
	if (!g_dbg_g4Fast || !s_g4LastResolved || !GTECapture_NodeExempt() || (GTECapture_NodeFlagSlot() == NULL) || GTECapture_NodePaintNear() ||
	    s_skyNodeActive || s_glowNodeActive || s_menubdNodeActive || s_hudshineNodeActive || s_uimapNodeActive || s_uifpsNodeActive || s_uipicNodeActive ||
	    (s_g4LastPerV[0] != s_g4LastStamp) || (s_g4LastPerV[1] != s_g4LastStamp) || (s_g4LastPerV[2] != s_g4LastStamp) ||
	    (s_g4LastPerV[3] != s_g4LastStamp))
	{
		return;
	}
	DrawEnvOffset(&ofsX, &ofsY);
	/* The generic path adds the float offset to the s16 coordinate; with a
	 * whole-number offset an integer add is the same value. */
	if ((ofsX != (float)(int)ofsX) || (ofsY != (float)(int)ofsY))
	{
		return;
	}
	c->ofsX = (int)ofsX;
	c->ofsY = (int)ofsY;
	c->suppressed = GTECapture_LookupSuppressed();
	c->slotIdx = s_gpu.otCurrentSlotIdx;
	c->stamp = s_g4LastStamp;
	c->z = (s16)((int)s_g4LastStamp - 32768);
	c->uiShift = (c->z >= 32704);
	c->u = (s16)(((N3DS_WHITE_CELL_X * N3DS_DECODE_TEXTURE_DIM + (N3DS_DECODE_TEXTURE_DIM / 2)) << 1) + 1);
	c->v = (s16)(2047 - ((N3DS_WHITE_CELL_Y * N3DS_DECODE_TEXTURE_DIM + (N3DS_DECODE_TEXTURE_DIM / 2)) << 1));
	c->valid = 1;
}

/* Whether the node the walk just marked is still in the seed's state. */
internal int DirectEmit_G4FastOk(const G4FastCtx *c)
{
	return c->valid && GTECapture_NodeExempt() && (GTECapture_NodeFlagSlot() != NULL) && (GTECapture_LookupSuppressed() == c->suppressed) &&
	       !GTECapture_NodePaintNear() && (s_gpu.otCurrentSlotIdx == c->slotIdx);
}

/* One flag quad: the generic walker's emit for a POLY_G4 with this context's
 * resolved depth. Same counters, ordinal advance and bytes. */
internal void DirectEmit_G4FastQuad(const u32 *words, const G4FastCtx *c)
{
	const u16 *slot = GTECapture_NodeFlagSlot();
	u32 xy[4], rgb[4];
	u16 par[4];
	N3DS_Vertex sv[4];
	int v, t;

	rgb[0] = words[0] & 0x00FFFFFFu;
	xy[0] = words[1];
	rgb[1] = words[2] & 0x00FFFFFFu;
	xy[1] = words[3];
	rgb[2] = words[4] & 0x00FFFFFFu;
	xy[2] = words[5];
	rgb[3] = words[6] & 0x00FFFFFFu;
	xy[3] = words[7];

	s_demit.framePrims++;
	s_demit.frameTris += 2u;
	(void)NativeGpu_NextDepthStamp();
	s_gpu.emitOrdinal++;
	NativeGpu_RecordContentAdvance();

	/* Parallax: the quad's own table depth for each vertex, matched by
	 * coordinate like GTECapture_NodeFlagDepth (first match wins); a vertex
	 * with none keeps the occlusion stamp. */
	for (v = 0; v < 4; v++)
	{
		int j;

		par[v] = c->stamp;
		for (j = 0; j < 4; j++)
		{
			if (xy[v] == xy[j])
			{
				par[v] = PacketDepth_ClampWorld(0xFFFFu - slot[j]);
				break;
			}
		}
	}

	for (v = 0; v < 4; v++)
	{
		s16 dx = (s16)((int)(s16)(xy[v] & 0xFFFFu) + c->ofsX);

		if (c->uiShift)
		{
			extern int NativeWide_UiShiftX(int x);

			dx = (s16)NativeWide_UiShiftX((int)dx);
		}
		sv[v].x = dx;
		sv[v].y = (s16)((int)(s16)(xy[v] >> 16) + c->ofsY);
		sv[v].z = c->z;
		sv[v].w = (s16)((int)par[v] - 32768);
		sv[v].u = c->u;
		sv[v].v = c->v;
		sv[v].r = (u8)(rgb[v] & 0xFFu);
		sv[v].g = (u8)((rgb[v] >> 8) & 0xFFu);
		sv[v].b = (u8)((rgb[v] >> 16) & 0xFFu);
		sv[v]._pad = 0;
	}

	for (t = 0; t < 2; t++)
	{
		N3DS_Vertex *dst;

		if ((s_mergeBinUsed[N3DS_MERGE_UNTEX_BIN] + 3) > N3DS_MERGE_BIN_CAP_VERTS)
		{
			N3DS_MergeSpillBin(N3DS_MERGE_UNTEX_BIN);
		}
		dst = &s_mergeBinVerts[N3DS_MERGE_UNTEX_BIN][s_mergeBinUsed[N3DS_MERGE_UNTEX_BIN]];
		dst[0] = sv[t];
		dst[1] = sv[t + 1];
		dst[2] = sv[t + 2];
		s_mergeBinUsed[N3DS_MERGE_UNTEX_BIN] += 3;
		s_demit.frameEmitTris++;
	}
	s_gpu.framebufferFeedbackRunActive = false;
}

#if defined(CTR_INTERNAL)
/* dumpreq 'g4check': every 8th flag quad is emitted by BOTH paths from the
 * same state and the six vertices compared; the generic walker's result is the
 * one kept (the fast path's side effects are rewound first). A line per 256
 * checks reports quads and mismatches. Skipped when the bin could spill. */
internal void DirectEmit_G4CheckQuad(const u32 *w, const G4FastCtx *c)
{
	const int bin = N3DS_MERGE_UNTEX_BIN;
	static u32 s_checked;

	if (((s_g4CheckQuads++ & 7u) != 0u) || ((s_mergeBinUsed[bin] + 6) > N3DS_MERGE_BIN_CAP_VERTS))
	{
		DirectEmit_G4FastQuad(w, c);
		return;
	}
	{
		const int used0 = (int)s_mergeBinUsed[bin];
		const __typeof__(s_gpu.emitOrdinal) ord0 = s_gpu.emitOrdinal;
		const __typeof__(s_gpu.lastStampedZ) lastZ0 = s_gpu.lastStampedZ;
		const u32 stamps0 = s_pktDepthStamps;
		const DirectEmitStats stats0 = s_demit;
		N3DS_Vertex ref[6];
		__typeof__(s_gpu.emitOrdinal) ordSlow;
		int bad;

		(void)DirectEmit_WalkNodeG4(w);
		memcpy(ref, &s_mergeBinVerts[bin][used0], sizeof(ref));
		ordSlow = s_gpu.emitOrdinal;

		s_mergeBinUsed[bin] = used0;
		s_gpu.emitOrdinal = ord0;
		s_gpu.lastStampedZ = lastZ0;
		s_pktDepthStamps = stamps0;
		s_demit = stats0;

		DirectEmit_G4FastQuad(w, c);
		bad = (memcmp(ref, &s_mergeBinVerts[bin][used0], sizeof(ref)) != 0) || (ordSlow != s_gpu.emitOrdinal);
		s_g4CheckBad += (u32)bad;
		if ((++s_checked & 255u) == 0u)
		{
			char line[96];

			snprintf(line, sizeof(line), "[G4CHECK] checked=%lu bad=%lu", (unsigned long)s_checked, (unsigned long)s_g4CheckBad);
			N3DS_DebugLog(line);
		}
	}
}
#endif

/*
 * Runtime A/B for the G4 run below (dumpreq 'g4run'); both legs produce the
 * same bins, so a flip mid-frame only splits that frame between them.
 */
int g_dbg_g4Run = 1;

/*
 * POLY_G4 run. The menu/loading flag (RaceFlag_DrawSelf) links its ~1000
 * quads one per OT node, back to back, so the walk's generic per-node work --
 * tag tests, the fast-path predicate, the shape decode, three function
 * frames -- costs more than the quad itself. Called by the walk right after
 * NativeDirectEmit_TryNode emitted `packet`: while the next node is another
 * one-prim POLY_G4 node the generic loop would hand to the same single-pass
 * walker, this does for it exactly what one generic iteration does -- link
 * validation, GTECapture_MarkOTNode, the tag test, the fast-path predicate,
 * the counters, DirectEmit_WalkNodeG4 -- and moves on. It stops BEFORE the
 * first node that needs anything else (tag range, UI band latch, another
 * shape, the end of the OT, a bad link, the cycle guard) and returns the last
 * node it emitted, so the walk's own tail and next iteration take over there
 * unchanged (MarkOTNode is idempotent for the node it re-marks). The node the
 * walk handed in must itself be outside every tag range, so the walk's tag
 * flags are all zero and stay zero across the run.
 */
internal u8 *NativeDirectEmit_G4Run(u8 *packet, u32 tagLength, u32 *walkIterations, u32 *runNodes)
{
	u8 *node = packet;
	G4FastCtx fctx;

	*runNodes = 0;
	fctx.valid = 0;
	if (!g_dbg_g4Run || g_dbg_perfTriScopes || (tagLength != DEMIT_LEN_POLY_G4) ||
	    ((((const u32 *)packet)[P_LEN] >> 24) != DEMIT_CODE_POLY_G4) || NativeGpu_InTagEnvelope((uintptr_t)packet))
	{
		return node;
	}

	DirectEmit_G4FastSeed(&fctx);
	for (;;)
	{
		u8 *next;
		int scan;

		if (isendprim(node))
		{
			return node;
		}
		next = nextPrim(node);
		if (!NativeGpu_IsValidOTLink((uintptr_t)next))
		{
			return node; /* the walk's tail logs it */
		}
		if ((getlen(next) != DEMIT_LEN_POLY_G4) || ((((const u32 *)next)[P_LEN] >> 24) != DEMIT_CODE_POLY_G4))
		{
			return node;
		}
		if (*walkIterations >= 65536)
		{
			return node; /* the walk's cycle guard reports it */
		}

		GTECapture_MarkOTNode(next);
		if (NativeGpu_InTagEnvelope((uintptr_t)next) || !g_dbg_directEmit || !DirectEmit_NodeActive())
		{
			return node;
		}

		(*walkIterations)++;
		node = next;
		s_demit.frameNodes++;
		{
#if defined(CTR_INTERNAL)
			const int g4Sampled = ((((*runNodes) % PARSEPROF_G4_SAMPLE_EVERY) == 0u));
			const u64 g4Start = g4Sampled ? PARSEPROF_TICK() : 0;
#endif
			if (DirectEmit_G4FastOk(&fctx))
			{
				const u32 *fw = (const u32 *)node + P_LEN;

#if defined(CTR_INTERNAL)
				if (g_dbg_g4Check)
				{
					DirectEmit_G4CheckQuad(fw, &fctx);
				}
				else
#endif
				{
					DirectEmit_G4FastQuad(fw, &fctx);
				}
				/* The resolve's own counters, as the generic walker leaves them. */
				if (fctx.suppressed)
				{
					s_pktDepth.suppressed2d++;
				}
				else
				{
					s_pktDepth.fellBack++;
					s_pktDepth2.unkFellBack++;
				}
				scan = DEMIT_OK;
			}
			else
			{
#if defined(CTR_INTERNAL)
				s_parseProfG4Sample = g4Sampled;
#endif
				scan = DirectEmit_WalkNodeG4((const u32 *)node + P_LEN);
#if defined(CTR_INTERNAL)
				s_parseProfG4Sample = 0;
#endif
			}
#if defined(CTR_INTERNAL)
			if (g4Sampled)
			{
				s_parseProf.g4NodeTicks += PARSEPROF_TICK() - g4Start;
				s_parseProf.g4Samples++;
			}
#endif
		}
		if (scan != DEMIT_OK)
		{
			/* Unreachable for an opaque untextured G4 of the right length;
			 * counted the way TryNode would, and the node is still handled
			 * here rather than parsed twice. */
			s_demit.frameSkip[scan]++;
		}
		else
		{
			s_demit.frameElig++;
		}
		(*runNodes)++;
	}
}
#endif

/*
 * Per content-frame fold + 64-frame window report, called from ClearSplits
 * (same cadence as [OTPROF]). Printed via Platform_Log so it reaches the 3DS
 * SD log -- the [OTPROF] console-only gap is exactly what this
 * line must not repeat, PC replays and hardware need the same instrument.
 */
internal void DirectEmit_FoldFrame(void)
{
	int i;

	if (s_demit.frameNodes == 0)
	{
		return;
	}

	s_demit.winFrames++;
	s_demit.winNodes += s_demit.frameNodes;
	s_demit.winElig += s_demit.frameElig;
	s_demit.winPrims += s_demit.framePrims;
	s_demit.winTris += s_demit.frameTris;
	s_demit.winNullPrims += s_demit.frameNullPrims;
	s_demit.winEmitTris += s_demit.frameEmitTris;
	for (i = 0; i < DEMIT_SKIP_COUNT; i++)
	{
		s_demit.winSkip[i] += s_demit.frameSkip[i];
	}

	s_demit.frameNodes = 0;
	s_demit.frameElig = 0;
	s_demit.framePrims = 0;
	s_demit.frameTris = 0;
	s_demit.frameNullPrims = 0;
	s_demit.frameEmitTris = 0;
	for (i = 0; i < DEMIT_SKIP_COUNT; i++)
	{
		s_demit.frameSkip[i] = 0;
	}

	if (s_demit.winFrames < DEMIT_WINDOW_FRAMES)
	{
		return;
	}

#ifdef __3DS__
	/* N3DS_DebugLog, NOT Platform_Log: on real hardware the Platform_Log app
	 * log file never gets created (netloaded runs have no writable cwd for
	 * the relative fopen), so its lines exist nowhere pullable -- measured on
	 * first netload: Azahar had the [DEMIT] stream, the console had
	 * none. Same gap class as [OTPROF]; ctr3ds.log is the sink that works. */
	{
		char line[224];

		snprintf(line, sizeof(line), "[DEMIT] f=%lu nodes=%lu elig=%lu (%lu%%) prims=%lu tris=%lu null=%lu emit=%lu skip env=%lu poly=%lu blend=%lu fmt16=%lu mal=%lu",
		         (unsigned long)s_demit.winFrames, (unsigned long)s_demit.winNodes, (unsigned long)s_demit.winElig,
		         (unsigned long)((s_demit.winNodes != 0) ? ((s_demit.winElig * 100u) / s_demit.winNodes) : 0u), (unsigned long)s_demit.winPrims,
		         (unsigned long)s_demit.winTris, (unsigned long)s_demit.winNullPrims, (unsigned long)s_demit.winEmitTris,
		         (unsigned long)s_demit.winSkip[DEMIT_SKIP_ENV], (unsigned long)s_demit.winSkip[DEMIT_SKIP_NONPOLY],
		         (unsigned long)s_demit.winSkip[DEMIT_SKIP_BLEND], (unsigned long)s_demit.winSkip[DEMIT_SKIP_FMT16],
		         (unsigned long)s_demit.winSkip[DEMIT_SKIP_MALFORMED]);
		N3DS_DebugLog(line);
	}
#else
	Platform_LogInstrument("[DEMIT] f=%lu nodes=%lu elig=%lu (%lu%%) prims=%lu tris=%lu null=%lu emit=%lu skip env=%lu poly=%lu blend=%lu fmt16=%lu mal=%lu\n",
	             (unsigned long)s_demit.winFrames, (unsigned long)s_demit.winNodes, (unsigned long)s_demit.winElig,
	             (unsigned long)((s_demit.winNodes != 0) ? ((s_demit.winElig * 100u) / s_demit.winNodes) : 0u), (unsigned long)s_demit.winPrims,
	             (unsigned long)s_demit.winTris, (unsigned long)s_demit.winNullPrims, (unsigned long)s_demit.winEmitTris,
	             (unsigned long)s_demit.winSkip[DEMIT_SKIP_ENV], (unsigned long)s_demit.winSkip[DEMIT_SKIP_NONPOLY],
	             (unsigned long)s_demit.winSkip[DEMIT_SKIP_BLEND], (unsigned long)s_demit.winSkip[DEMIT_SKIP_FMT16],
	             (unsigned long)s_demit.winSkip[DEMIT_SKIP_MALFORMED]);
#endif

	memset(&s_demit, 0, sizeof(s_demit));
}

#endif /* defined(CTR_NATIVE) && CTR_NATIVE_DIRECT_EMIT */
