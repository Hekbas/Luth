# material-system.M.3 — cutout-rt

**Date:** 2026-06-11
**Commits:** on `feat/cutout-rt` — `9279848` (TLAS flag), `6b53d93` (helper), `3730c80` (PT/GI/refl), `74070a6` (ReSTIR-DI), `2281ff6` (volumetric), `c01a933` (sun-shadow rewrite), wrap-up
**Umbrella:** [#151](https://github.com/Hekbas/Luth/issues/151)
**Series:** `material-system`, M.3. Mode A — **v3.1.2** PATCH bump, tag-only.

---

## Overview

The RT path was opaque-only: every rayQuery forced `gl_RayFlagsOpaqueEXT`, so alpha-cutout materials
(`Material::RenderMode::Cutout` — foliage, fences, chain-link) occluded/reflected as **solid** in RT
shadows, GI, reflections, ReSTIR DI, and volumetric fog. Raster already alpha-tested them (`pbr.frag`:
`if (albedo.a < alphaCutoff) discard`). M.3 closes the RT opaque-only deferral so RT matches raster — leaf
holes in every ray-traced consumer, including the headline **holed primary sun shadow on the floor**.

Smoke-testing ST6 surfaced the *other half* of the problem: cutout surfaces also **receive** shadows wrong.
Cutout was never written into the slim G-buffer (opaque-only depth prepass + EQUAL slim test), so the
sun-shadow pass reconstructed world-pos/normal from the opaque surface *behind* the cutout — farther
objects' shadows landed on the cutout card. ST8 fixes the receiving side by treating cutout as
alpha-tested-opaque in the slim G-buffer (so RT shadows, reflections, GTAO, and TAA motion all see it).

### Deviation from the ROADMAP wording (documented divergence)

The planned-epics row read *"per-instance non-opaque BLAS + anyhit alpha."* The engine is **rayQuery-centric**
(8 of 9 ray sites are rayQuery-in-compute), so two deliberate departures:

1. **Opaqueness via a per-instance TLAS flag, not a non-opaque BLAS rebuild.** `cutout ? FORCE_NO_OPAQUE :
   FORCE_OPAQUE` on the instance unifies static + skinned with zero BLAS change. Vulkan precedence is
   ray > instance > geometry, so the static BLAS `VK_GEOMETRY_OPAQUE_BIT_KHR` becomes a redundant no-op.
2. **Alpha-test in rayQuery candidate loops, not anyhit shaders.** One shared helper (`geom_table.glsl`)
   serves all eight rayQuery sites. The lone RT-*pipeline* site (`rt_sun_shadows`) was **rewritten to
   rayQuery-compute** rather than given an anyhit shader — it does compute-shaped work anyway, and the
   rewrite *deletes* the SBT/rgen/rmiss surface instead of adding a second alpha-test dialect. After M.3
   there are zero production RT-pipeline consumers (only the validation smoke test, with empty layouts).

No cornerstone impact: no new allocator/sync primitive; composes with the existing TlasBuilder, the
per-frame geometry table, and the proven 5-set layout (Set 0 TLAS / Set 3 Material / Set 4 bindless).

---

## Mechanism

- **`FORCE_OPAQUE` is load-bearing, not just perf.** Once the rays drop `gl_RayFlagsOpaqueEXT`, opaque +
  skinned geometry (BLAS `flags=0`) would otherwise yield a candidate for *every* triangle. Forcing opaque
  on all non-cutout instances keeps them hardware-auto-confirmed.
- **Dirty-hash fold.** `TlasBuilder::HashInstances` folds each instance's `GetRenderMode()` (resolved via
  `AssetManager::GetAsset<Material>` by UUID), so an in-place opaque↔cutout edit on a stable mesh rebuilds
  the TLAS the same frame.
- **Shared helper** `AlphaTestCandidateHit(GeomTable, ci, pi, bary)` in `common/geom_table.glsl`:
  interpolate the diffuse UV (UV0/UV1 by the flags' per-map index), `alpha = color.a * (HAS_DIFFUSE ?
  texture(diffuse).a : 1)`, keep when `alpha >= alphaCutoff`. Matches raster exactly; `alphaCutoff <= 0`
  (non-cutout) always passes — doubly safe with FORCE_OPAQUE. `RtConfirmAlphaCandidates(rq, geomTable)`
  drives the query: `while (proceed) if (candidateTri && AlphaTestCandidateHit) confirm;`.
- **Two candidate-loop shapes.** Shadow rays keep `TerminateOnFirstHit`, drop `Opaque`, occluded = committed
  != None. Closest-hit rays use `gl_RayFlagsNoneEXT`, confirm-on-pass; the committed hit is the nearest
  alpha-passing one (existing `FetchHitSurface(rq, true)` reads unchanged).

---

## Sub-tasks

| # | What landed | Commit |
|---|---|---|
| ST1 | **Per-instance TLAS opaque flag.** `TlasBuilder.cpp` instance `.flags` = `cutout ? FORCE_NO_OPAQUE : FORCE_OPAQUE`, resolving `GetRenderMode()` alongside the existing `materialSlot` lookup; folded RenderMode into `HashInstances` via `GetAsset<Material>`. Byte-identical output while the rays still force OPAQUE everywhere. | `9279848` |
| ST2 | **`AlphaTestCandidateHit` helper** in `common/geom_table.glsl` (dead fn at this point; compiles via its three includers). | `6b53d93` |
| ST3 | **Candidate loops + drop `Opaque`** in `path_trace.comp` (2 sites), `restir_gi_initial.comp` (3), `rt_reflections.comp` (2). Added `RtConfirmAlphaCandidates`. **First-visible milestone** — cutout holes in PT primary/shadows, GI bounce + NEE, reflections + their shadows. Shader-only. | `3730c80` |
| ST4 | **ReSTIR DI visibility.** Grew `m_InitialPipeline` to the 5-set layout (Set 3 Material + Set 4 bindless) + `RestirPC.geomTableBDA`; candidate loop in `restir_initial.comp`. Temporal/spatial/shade trace no rays → unchanged. | `74070a6` |
| ST5 | **Volumetric fog.** Same 5-set + BDA on the inject-scatter pipeline; candidate loop in `volumetric_inject_scatter.comp`'s `Visible()`. The pipeline has **no pass-local Set 2**, so an empty placeholder layout fills index 2 ([global, scatter, EMPTY, Material, bindless]); two binds straddle it (firstSet 0 count 2, firstSet 3 count 2). `geomTableBDA` appended to the shared `InjectPC` (88 B). | `2281ff6` |
| ST6 | **Sun-shadow rewrite (the L one).** `rt_sun_shadows.rgen`+`.rmiss` → `rt_sun_shadows.comp` on `VKComputePipeline`; depth→worldPos→sun ray→R8 mask. 5-set layout + a `GeomTable` push constant + the candidate loop. Deleted the rgen/rmiss/SBT/RT-pipeline; flipped the AS-build→read barrier dst `RAY_TRACING`→`COMPUTE_SHADER`; pass-local Set 2 stage flags `RAYGEN`→`COMPUTE`; dropped now-dead RAYGEN bits on the global UBO + light SSBO layouts. | `c01a933` |
| ST7 | **Wrap-up.** `Version.h` → 3.1.2, ROADMAP M.3 ✅ + v3.1.2 row, this history file, `--no-ff` merge to local `main` + tag `v3.1.2`. | wrap-up |
| ST8 | **Cutout in the slim G-buffer (smoke-found, the receiving side).** `slim_gbuffer.frag` alpha-tests (mirrors pbr.frag, no-op for opaque). A cutout slim pipeline (`LESS_OR_EQUAL` + `depthWrite=true`) writes cutout depth + normal into SceneDepth/SlimNormal so the sun-shadow pass (+ RT reflections + GTAO + TAA motion) reconstructs from the holed cutout surface, not the opaque geometry behind it. Opaque slim path byte-for-byte untouched. | `741c043` |
| ST9 | **Two-sided cutout back faces (smoke round 2).** Cull-None foliage looked unlit + see-through from behind: the slim cutout pipeline still culled back faces (so they were absent from the slim G-buffer → RT effects read the geometry behind), and neither `pbr.frag` nor `slim_gbuffer.frag` flipped the normal on back faces. Fix: slim cutout pipeline → `CULL_NONE`; both shaders flip the shading normal on `!gl_FrontFacing` (matching the RT path, which faces its normal to the ray — why PathTrace foliage was already correct). | `bb25c36` |

---

## Design decisions

### Skinned cutout needs no special case
The instance flag overrides the BLAS `flags=0` by Vulkan precedence, and skinned UVs are bind-pose-stable
(skinning moves positions, not UVs) — so the bind-pose attribute read in `geom_table.glsl` alpha-tests
exactly. Animated foliage casts holed shadows while deforming with no extra path.

### `rt_sun_shadows`: rewrite, not retrofit
It was the only `VKRayTracingPipeline` in the engine. Adding an anyhit shader for cutout would have grown
the SBT (a hit group with an anyhit stage) and introduced a *second* alpha-test dialect (GLSL anyhit vs the
rayQuery candidate loop). Rewriting to rayQuery-in-compute instead **deletes** the rgen/rmiss/SBT and reuses
the exact shared helper. The barrier dst must flip `RAY_TRACING_SHADER`→`COMPUTE_SHADER` — a rayQuery in
compute that waits on a `RAY_TRACING` stage is the documented TDR trap (same as the volumetric + DI passes).

### Volumetric's empty Set 2
`geom_table.glsl` hardcodes Set 3 (Material) / Set 4 (bindless). Most consumers already have a pass-local
Set 2, but the inject-scatter pipeline uses only Set 0 (global) + Set 1 (scatter state). Rather than
renumber the shared include, a 0-binding placeholder layout fills index 2; it is never bound (the shader
never references Set 2). Set 3/4 are bound every dispatch even when RT fog is off, because the shader
*statically* references them via the include and validation requires bound sets for static references.

### `textureLod` vs mipped — documented edge-parity gap
The helper samples the diffuse alpha at LOD 0 (`textureLod(.,0)`); raster's `texture()` is mipped. This is a
minor sub-texel silhouette difference at distance, not a correctness bug, and is the standard RT-cutout
tradeoff (mip selection needs ray differentials the rayQuery path doesn't carry).

### Always bind + gate in shader
Set 3/4 (and the geom-table BDA) bind on every shadow/scatter dispatch regardless of the RT-shadow toggle,
matching the PathTrace reference. The BDA is valid whenever rays actually trace (the TLAS + geometry table
build in lockstep behind the same `IsRtShadowsEnabled()` gate); a 0 BDA is never dereferenced because the
candidate loop only runs inside the gated `Visible()`/trace path.

### Cutout in the slim G-buffer is the receiving-side fix (ST8)
The RT shadow/reflection consumers reconstruct a world position + normal from the slim G-buffer (depth +
oct-normal). Cutout was excluded from both the opaque depth prepass and the EQUAL slim test, so at cutout
pixels they read the *opaque surface behind the holes* — farther shadows bled onto the cutout card. The fix
treats cutout as **alpha-tested-opaque** in the slim pass: a dedicated cutout pipeline tests `LESS_OR_EQUAL`
(the opaque-only prepass left those pixels at the clear/behind depth) and **writes** depth + normal, while
`slim_gbuffer.frag` runs the same `color.a × diffuse.a < alphaCutoff` discard as `pbr.frag`. The opaque slim
pipeline (EQUAL, no depth write) stays byte-for-byte unchanged — cutout is purely additive, and it gives
cutout correct GTAO + TAA motion for free. The depth prepass stays opaque-only: nothing reads SceneDepth
between the prepass and the slim pass, so writing cutout depth in the slim pass reaches every downstream
consumer.

**Two-sided (ST9).** Cutout foliage is typically `CullMode::None`, so the cutout slim pipeline is `CULL_NONE`
(not the opaque `CULL_BACK`) — otherwise back faces are culled out of the slim G-buffer and RT effects read
the geometry behind the leaf. Both `pbr.frag` and `slim_gbuffer.frag` flip the shading normal on
`!gl_FrontFacing` so back faces light correctly; the RT path needs no such flip — `FetchHitSurface` already
faces the geometric normal against the ray (which is why PathTrace foliage was right from ST3).

---

## Files touched

**Shaders.** `common/geom_table.glsl` (ST2 helper + `RtConfirmAlphaCandidates`), `path_trace.comp`,
`restir_gi_initial.comp`, `rt_reflections.comp` (ST3 loops), `restir_initial.comp` (ST4),
`volumetric_inject_scatter.comp` (ST5), new `rt_sun_shadows.comp` (+`.meta`) replacing `rt_sun_shadows.rgen`
+ `.rmiss` (ST6), `slim_gbuffer.frag` (ST8 alpha test + ST9 back-face normal flip), `pbr.frag` (ST9
two-sided normal flip).
**C++.** `backend/vulkan/TlasBuilder.cpp` (ST1), `RtRestirSubsystem.cpp` (ST4),
`VolumetricSubsystem.{cpp,h}` (ST5), `RtSubsystem.{cpp,h}` + `GlobalSubsystem.cpp` + `LightingSubsystem.cpp`
+ `ViewResources.cpp` (ST6), `GeometrySubsystem.{cpp,h}` (ST8 cutout slim pipelines + draw loop; ST9 `CULL_NONE`).
**Docs.** `core/Version.h` (3.1.2), `ROADMAP.md`, `arch/rendering-pipeline.md` (slim-pass flow), this file.

---

## Verification

Per ST: `glslc --target-env=vulkan1.3 -I luth/assets/shaders` on every touched/including shader; Debug x64
MSBuild for the C++ STs (ST1/ST4/ST5/ST6/ST8/ST9). The shared `static_assert(sizeof(InjectPC) == 88)` guards
the volumetric push-constant layout.

**Smoke (user), against PathTrace as parity gold standard:** a cutout-foliage card (`alphaCutoff≈0.5`) —
ST1–ST4 verified 2026-06-10 (cutout holes correct in PathTrace / reflections / GI / DI). ST5 leaf-shaped
god-rays in a fog volume; ST6 **holed primary sun shadow on the floor** (the headline). ST8 closes the
receiving side: in normal mode the cutout surface samples its *own* depth/normal, so farther geometry's
shadows no longer bleed onto the card (the bug ST6 smoke surfaced). ST9 cull-None foliage lights + occludes
correctly from both sides (no unlit/see-through back face). Opaque scenes render identically pre/post with no
RT perf regression; animated skinned cutout casts holed shadows while deforming.

---

## Hand-off / deferred

- **M.4 `transparency-tier`** is next (back-to-front sort in DrawListBuilder; WBOIT only if layered;
  RT-excluded; resolves bug [#32](https://github.com/Hekbas/Luth/issues/32)).
- **Local-only per the active workflow:** v3.1.0–v3.1.2 sit merged + tagged on local `main`, unpushed; the
  milestone Release decision (and the push of the whole material-system arc) is still open.
- Deferred elsewhere in the arc: emissive-as-area-lights; a Slang `IMaterial` spike. `gpu-particles`
  ([#57](https://github.com/Hekbas/Luth/issues/57)) is parallelizable, not a material-system dependency.
- Git hooks installed in this workspace — comment/commit policy enforced mechanically.
