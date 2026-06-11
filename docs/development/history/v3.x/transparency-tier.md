# material-system.M.4 — transparency-tier

**Date:** 2026-06-11
**Commits:** on `feat/transparency-tier` — `4f8032a` (shader seams), `37d4c2a` (TLAS/shadow exclusion), `44dbc20` (sorted pass), `e609ac9` (PPLL infra), `a5a55dc` (PPLL passes), `59b40e1` (wrap-up docs), + cull-mask visibility (post-smoke-gate)
**Umbrella:** [#151](https://github.com/Hekbas/Luth/issues/151) · **Closes:** [#32](https://github.com/Hekbas/Luth/issues/32)
**Series:** `material-system`, M.4. Mode A — **v3.1.3** PATCH bump, tag-only.

---

## Overview

Bug #32: Transparent/Fade meshes drew inside `GeometryPass` *before* `SkyboxPass`, blending against the
clear color instead of the sky, and the transparent bucket was never sorted. Worse, the shared `pbr.frag`
fed them four opaque-depth-coupled screen-space inputs (RT sun-shadow mask, ReSTIR DI, ReSTIR GI, GTAO) —
all wrong at any depth other than the opaque surface — and transparent instances were packed into the TLAS
as `FORCE_OPAQUE`, so glass acted as a **solid occluder** for every RT consumer and drew into the CSM
cascades.

M.4 builds the transparency *tier*: a dedicated pass slot after the volumetric composite with two runtime
modes — classic per-view back-to-front **Sorted** blending, and **PPLL OIT** (default): a per-pixel
linked-list store + fullscreen sorted resolve giving exact layering at any opacity, up to a budgeted node
pool. Transparent shading is corrected end-to-end (per-fragment rayQuery sun shadow, cluster point-light
loop, IBL ambient, froxel fog sampled at the *fragment's* depth), and transparent geometry is fully
RT-excluded (TLAS + shadow batches).

### Scope override (user-directed)

The ROADMAP row read *"Back-to-front sort in DrawListBuilder; WBOIT only if layered"*. The user overrode:
true OIT in scope from the start, technique chosen by a research pass (Interplay-of-Light OIT survey,
NVIDIA `vk_order_independent_transparency`, MJP's WBOIT analysis, Bevy 0.15 OIT, UE5/Godot status):

- **PPLL chosen** — exact layering up to the node budget; single shaded geometry pass + resolve; core-Vulkan
  atomics (no `VK_EXT_fragment_shader_interlock` dependency); the node pool composes directly with the
  Garlic `AllocateLargeTaggedDeviceLocal` reserved-tag primitive (ReSTIR-reservoir lifecycle).
- **WBOIT rejected** — order-independent but approximate: high-opacity stacks wash out, the depth-weight
  needs per-scene tuning, and artifacts amplify with HDR intensity contrast between layers.
- **MBOIT rejected** — two transparent geometry passes through the indirect path + moment/bias tuning, at
  near-PPLL complexity without exactness.
- **Sorted mode retained** as the A/B fallback (ShadowingMode-toggle precedent) and the issue-#32 sort item.

### Deviations from the issue checklist (documented divergence)

1. **Pass runs after `VolumetricCompositePass`, not directly after Skybox** — glass blends over the fogged
   background; its own fog is applied per-fragment at the glass depth (the composite runs at opaque depth
   and cannot know glass depth — glass writes no depth).
2. **Sort lives at pass level per-view, not in DrawListBuilder** — one shared DrawList serves the Scene and
   Game views; camera distance is view-dependent. The indirect-slot offset is keyed by `dc.gpuObjectIndex`
   (not iteration position), so a per-view sorted *index array* (LinearAllocator scratch) reorders
   submission with zero SSBO/cull changes and no in-place mutation of the shared bucket.

No cornerstone deviations: every new mechanism composes with an existing primitive (Garlic large allocs,
ViewResources per-view images, the RG pass model + two additive ResourceStates, LinearAllocator scratch,
PipelineManager variants, the `geom_table.glsl` rayQuery seam, the froxel mapping).

---

## Sub-tasks

| # | Commit | What |
|---|--------|------|
| ST1 | `4f8032a` | `refactor(shaders)`: pbr.frag's material decode → `common/pbr_surface.glsl` (`EvalPbrSurface`, discard-after-albedo order preserved); CSM + cluster loop + IBL → `common/pbr_lighting.glsl`. Zero visual change; serves the arc's eval-seam goal (three consumers, one body) |
| ST2 | `37d4c2a` | `fix(rt)`: TlasBuilder pack loop skips Transparent/Fade (`instanceCustomIndex` stays 1:1 — skip precedes `packed.push_back`); CSM ShadowPass + debugger shadow replay drop the transparent batch |
| ST3 | `44dbc20` | `feat(renderer)`: `TransparencySubsystem` (Set 6 layout, sorted + skinned PipelineManagers, per-view sort, DrawBatch-shaped pass after the fog composite); `pbr_transparent.frag` + shared `pbr_transparent_shading.glsl`; `froxel.glsl` extracted from the composite; `geom_table.glsl` gains the `GT_NO_RESOURCE_DECLS` alias guard; `TransparencySettings` + RenderPanel header; GeometryPass + geometry replay drop transparent |
| ST4 | `e609ac9` | `feat(renderer)`: RG `FragmentStorageRead/Write` states + fragment/transfer builder methods; `PipelineConfig` configurable color blend factors (defaults byte-identical); per-view `oitHeads` (R32_Uint storage) + Garlic node pool (reserved tags `0xFFFFC000+`, realloc on budget change); Set 6 b1/b2 + resolve-set writes; `oit_common.glsl` |
| ST5 | `a5a55dc` | `feat(renderer)`: `pbr_oit_store.frag` (early_fragment_tests + atomics push) + `oit_resolve.frag` (K-nearest insertion sort + tail merge + `ONE/SRC_ALPHA` under-composite + nearest-entity → EntityID); OITClear/Store/Resolve passes; default mode flips to OIT; hot-reload for both shaders |
| ST6 | `59b40e1` | wrap-up — arch docs (pass order, Set 6 row, transparency-tier note, Garlic consumer + reserved ranges), ROADMAP M.4 ✅ + completed row, this file, `Version.h` → 3.1.3, CLAUDE.md version line |
| ST7 | post-gate | `feat(rt)`: per-ray-class TLAS visibility — transparent re-packs with the GLASS mask + FORCE_OPAQUE; 8 shadow-class rayQuery sites cull to `GT_VIS_SOLID`, 3 world-class sites trace `GT_VIS_ALL`; restores emissive-glass GI + reflections presence (smoke-gate finding) |

---

## Mechanism

- **Node format (16 B):** `{ colorRGB9E5, depthAlpha, entityID, next }`. `depthAlpha = (fixed24(z) << 8) |
  alpha8` — one u32 sort key, ascending = nearest-first, alpha bits only break exact-depth ties. The plan's
  original packing put alpha+entity24 in one word; that truncates entt handles (version bits) and would
  break picking — full 32-bit entity kept instead, alpha folded into the key. RGB9E5 carries the HDR
  radiance (shared exponent, no negatives; radiance is fogged at store time, so each layer fogs at its own
  depth).
- **Store:** `early_fragment_tests` (legal with the alpha-cutoff discard because the pipeline never writes
  depth) → shade once via the shared body → `atomicAdd` on the pool counter → capacity check (overflow =
  fragment dropped) → `imageAtomicExchange` head publish → node field writes. No same-pass reader exists;
  the resolve is fenced by the graph.
- **Resolve:** walk the list (1024-iteration TDR guard), insertion-sort the K nearest
  (`maxResolveK ≤ 16`), merge deeper fragments order-independently into a tail slab
  (`tailColor += c·a; tailTrans *= (1−a)` — attenuated behind the exact layers, so the approximation
  hides), under-composite front-to-back, output `(C, T)` with blend `src=ONE, dst=SRC_ALPHA`;
  `discard` on empty pixels preserves both attachments. EntityID gets the nearest node's entity —
  integer-format attachments auto-disable blending (existing `VulkanPipeline` rule), so no
  per-attachment blend control was needed.
- **Cross-frame WAR rule (new RG insight):** heads/nodes are reused across frames; OITClear imports them in
  their END-of-frame state (`FragmentStorageRead`, GENERAL) so the clear's barrier orders after last
  frame's resolve reads. An `Undefined` import would carry srcStage TOP and race them. The heads image is
  bootstrap-transitioned to GENERAL (cleared to `OIT_EMPTY`) in the ViewResources one-shot clear block so
  the frame-0 claim holds.
- **New RG states:** `FragmentStorageRead/Write` — GENERAL layout, FRAGMENT stage; the write state carries
  `SHADER_READ` too (atomics are RMW). The pre-existing `Compute*` states emit COMPUTE-stage barriers and
  would under-synchronize fragment-stage storage (a real hazard, not conservatism).
- **Transparent sun shadow:** 1-spp alpha-tested rayQuery per fragment — the exact `rt_sun_shadows.comp`
  recipe (Wächter-Binder origin bias from `rtShadowParams.y/.z`, `TerminateOnFirstHit`, no opaque flag,
  `RtConfirmAlphaCandidates`) evaluated at the glass position. Set 0 b6 already carried `FRAGMENT_BIT`
  stage flags, and `RtSubsystem`'s **persistent empty TLAS** makes the static read always legal — rays
  simply miss before the first build (the geometry-table BDA is 0 then, and candidates can't arise).
  CSM mode uses `ComputeShadowCSM` unchanged (world-space — depth-correct anywhere).
- **`geom_table.glsl` set collision:** its material/texture decls sit at set 3/4 — occupied by Light/Bones
  in the geometry layout. `GT_NO_RESOURCE_DECLS` + `#define GtMaterial GPUMaterialData` /
  `gtMaterials materials` / `gtTextures globalTextures` alias them onto the pbr_surface seam
  (field-for-field identical, both 80 B std430); all existing consumers compile byte-identically.
- **Fog contract:** `final = radiance·(1−fogOpacity) + scatter`, `fogOpacity = clamp(1−T, 0,
  max(dfMaxOpacity, 0.001))` — algebraically the composite's blend (its `/fogOpacity · fogOpacity`
  cancels), evaluated at the fragment's froxel slice via `common/froxel.glsl` (math extracted
  byte-identical from `volumetric_composite.frag`).

## Memory

Per view at 1080p: heads 8.3 MB + node pool `16 + W·H·budget·16` B (~133 MB at the default
`avgLayersBudget = 4`; knob 1-16 in the Render panel, reallocates live via the `oitLayersCached`
recreate condition). Reserved Garlic tag range `0xFFFFC000+`, freed only on resize/budget/release —
the ReSTIR-reservoir lifecycle exactly.

## RT visibility revision (post-smoke-gate)

The first smoke pass caught two regressions from the strict ST2 TLAS exclusion: **emissive transparent
stopped lighting neighbors** (emissive feeds the scene through the ReSTIR GI bounce — no
emissive-as-area-lights yet — and GI rays could no longer hit glass) and **glass vanished from
reflections** (flagged at wrap-up, but "nothing" reads worse than the old opaque blob). The fix is
per-ray-class TLAS visibility via instance **cull masks** (`GT_VIS_SOLID 0x01` / `GT_VIS_ALL 0x03`,
mirrored in TlasBuilder): transparent instances pack with mask `GLASS (0x02)` + `FORCE_OPAQUE`;
shadow-class rays (sun shadows, ReSTIR-DI visibility, GI-bounce NEE ×2, reflection NEE, PT NEE, fog
shadows, the transparent fragment's own sun ray — 8 sites) trace `GT_VIS_SOLID`, so glass still never
blocks light; world-class rays (GI bounce, reflection ray, PT bounce — 3 sites) trace `GT_VIS_ALL`
and auto-confirm glass as a surface. Restores emissive-glass GI glow and glass-in-reflections
(unblended surface approximation) while keeping every shadow fix.

## Behavior changes (flagged for smoke test)

- Glass casts **no** shadows (CSM or RT) and never blocks light rays (cull-masked out of every
  shadow-class ray).
- Glass **appears in** RT reflections / GI / PT as an opaque-ish emissive surface (committed hit —
  not blended with what's behind it); emissive glass lights neighbors through the GI bounce again.
  True blended/refractive RT transparency remains a future follow-up.
- ShadeMode debug overrides on transparent (Unlit/Normals/EntityID/Emission) keep their pre-split
  alpha-1.0 look.

## Known limitations / deferred

- Overflow debug stat (counter readback → Render panel) — deferred; overflow currently drops fragments
  silently past the budget.
- Per-draw *replay* for the new passes (frame-debugger scrub) — archive-fallback only, like every
  non-Geometry pass; OITStore still captures per-draw entries.
- OITHeads frame-debugger preview RT — deferred (named-texture registry is FrameTargets-shaped; per-view
  registration needs its own design).
- Blue-noise slice dither on per-fragment glass fog — omitted (composite-only nicety); revisit if
  log-slice banding shows on large glass planes.
- Transparent writes no slim G-buffer motion vectors (pre-existing) — TAA reprojects glass via dilated
  opaque motion; ghosting on fast-moving glass is expected and unchanged.
- Tokuyoshi spec-AA omitted on the transparent path (opaque-path nicety).

## Files

**New:** `renderer/subsystems/TransparencySubsystem.{h,cpp}`, `renderer/settings/TransparencySettings.h`,
`shaders/pbr_transparent.frag`, `shaders/pbr_oit_store.frag`, `shaders/oit_resolve.frag`,
`shaders/common/{pbr_surface,pbr_lighting,pbr_transparent_shading,froxel,oit_common}.glsl`.
**Modified:** `pbr.frag`, `volumetric_composite.frag`, `geom_table.glsl`, `RenderPipeline.{h,cpp}`,
`ViewResources.cpp`, `RenderGraph.{h,cpp}`, `RenderGraphResources.h`, `VulkanPipeline.{h,cpp}`,
`GeometrySubsystem.cpp`, `LightingSubsystem.cpp`, `FrameDebuggerContext.cpp`, `TlasBuilder.cpp`,
`PickingSystem.cpp`, `RenderingSystem.h`, `luthien/panels/RenderPanel.cpp`.
