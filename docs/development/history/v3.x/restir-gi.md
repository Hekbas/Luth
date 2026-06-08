# rt-renderer.C.3 — restir-gi

**Date:** 2026-06-08
**Commits:** 8 on `feat/restir-gi` (S0 `b436f23`, S1 `4bae206`, S1-fix `37db437`, S2 `70bceec`, S3 `cd2e137`, S4 `9eda194`, S5 `00c734c`, wrap-up)
**Umbrella:** [#127](https://github.com/Hekbas/Luth/issues/127) (no per-effort issue; commits trail `Part of #127`)
**Series:** `rt-renderer`, Phase C.3. Mode A series-coalesced, **v3.0.14** tag-only, no Release.

---

## Overview

**ReSTIR GI** (Ouyang 2021) — 1-bounce indirect-diffuse global illumination via per-pixel *path* reservoirs, spatiotemporally resampled like C.1's DI reservoirs but **with a reconnection Jacobian** (the piece DI skipped: DI stores a light index → identity shift; GI stores a world-space sample point → a non-trivial Jacobian). Denoised by a second instance of C.2's SVGF, remodulated in `pbr.frag` alongside DI under `restirParams.y`.

Pass chain (all `AsyncCompute`, between the TLAS build and `GeometryPass`): **`restir_gi_initial`** (cosine-hemisphere 1-bounce → rayQuery commit-hit → secondary-hit `L_o` → reservoir) → **`restir_gi_temporal`** (motion reproject + reconnection Jacobian + Bitterli M-cap) → **`restir_gi_spatial`** (k-neighbour disk + Jacobian + RTXDI BASIC bias correction) → **`restir_gi_shade`** (demodulated `E = L_o·NdotL·W`) → **second SVGF instance** → `pbr.frag` remodulates `E·albedo·(1-metallic)/π`.

Like C.2, this effort leaned on **adversarial verification** for the unverifiable-by-eye parts: shader-math reviews after S1 + S2 (the Jacobian, BASIC, target function), and integration reviews after S3 + S4 (the TLAS contract change, bindless plumbing, the 2nd-instance channel parameterization). Each integration review caught real bugs (below).

---

## Sub-tasks

| # | What landed | Commit |
|---|---|---|
| S0 | **GI reservoir + initial 1-bounce + shade (scaffold `L_o`).** New `RtRestirGiSubsystem` (4 compute pipelines, pass-local Set 2 mirroring DI's 7 bindings, AS-build→read barrier `dst=COMPUTE`, `frameAbs&1` ping-pong, `NextReservoirTag` base `0xFFFF8000`); new 64 B `GIReservoir` (`restir_gi_common.glsl`) self-carrying receiver depth+normal; device-local Garlic ping-pong + spatial buffer; `restirParams.y` gate + Set 3 b6 sampler + `pbr.frag` remod. Scaffold `L_o` = constant albedo + receiver-facing normal. | `b436f23` |
| S1 | **Temporal reuse + reconnection Jacobian** (Ouyang Eq. 11, crossed ratios, both cosines at the sample point, reject [1/10,10] → clamp [1/3,3]). Motion reproject + self-carried-geometry validation + age cap + Bitterli M-cap. | `4bae206` |
| S1-fix | **Cross-frame reservoir read** (the churn bug). `prev` reservoir was imported `Undefined` → `srcAccessMask=0` → no availability op → frame N's write never made visible → 1-spp churn. Fix: import `prev` as `StorageBufferWrite` (its true last-left state). Also: self-carried packed normal must be a `uint` SSBO field via `packHalf2x16` (a `float` round-trip canonicalizes denormals → corrupt history). | `37db437` |
| S2 | **Spatial reuse + RTXDI BASIC bias correction** (`pi/piSum`, not biased 1/M). k-neighbour disk merge, Jacobian per neighbour (orig=neighbour, new=this pixel), geometry reject + J validate/clamp; reads temporal-out read-only, writes a separate spatial-out. | `70bceec` |
| S3 | **Real secondary-hit material** (geometry table + TLAS contract change). Per-frame geometry-table SSBO built in `TlasBuilder` in lockstep with the packed instances; `instanceCustomIndex` repurposed (unused entity id → table index). `restir_gi_initial.comp` deref's the hit triangle via `GL_EXT_buffer_reference2` → barycentric UV + world-space geometric normal → bindless Material SSBO (set 3) + textures (set 4). Table BDA rides the `GiPC` push constant, paired with `GetTlas()` at preflight (never desyncs). Scaffold kept behind `GI_USE_SCAFFOLD_LO`. | `cd2e137` |
| S4 | **Second SVGF instance + denoise wiring.** Channel-parameterized `SvgfDenoiser` (`DenoiserChannel{Di,Gi}` ctor + file-local `Resolve(channel,vr)` picking `svgf*` vs `svgfGi*` ViewResources + `GetSvgf[Gi]Settings` + distinct pass names). Second `m_DenoiseGi` instance; Execute feeds `giDIHandle → denoise → GeometryPass`; Set 3 b6 re-routed to `svgfGiDenoised`. Flat `svgfGi*` history images + 7 GI denoiser sets (pool +7/+17/+25). Separate `m_SvgfGiSettings`. Shaders unchanged (channel-agnostic). | `9eda194` |
| S5 | **Editor tuning.** RenderPanel "ReSTIR GI" + "SVGF (GI)" sections (mirror DI/SVGF). `RestirGiSettings` needed no new fields (plan's "final defaults" were already the S0-S2 values). raw/denoised A/B rides the SVGF-GI enable toggle. | `00c734c` |

---

## Design decisions

### Full per-material secondary color is IN (S3), staged after the resampling core
The reservoir/Jacobian/reuse/denoise/remodulation are all `L_o`-agnostic, so the material fetch is additive — but the TLAS-contract change it needs (`instanceCustomIndex`) is correctness-sensitive and touched once. So S0-S2 validated the novel ReSTIR math against a constant-albedo scaffold; S3 swapped in the real fetch. The scaffold survives as a `#define GI_USE_SCAFFOLD_LO` debug/fallback.

### Geometry table built in `TlasBuilder`, lockstep with the packed instances
`instanceCustomIndex` is set to the packed index immediately before the matching `geomEntries.push_back`, so the table is 1:1 with the TLAS — one source of ordering truth, not two. The table (host-visible SSBO, 24 B `{vertexBDA, indexBDA, materialSlot, vertexStride}` per instance) shares the TLAS lifetime exactly (kept across hash-skipped frames, freed on rebuild + shutdown).

### Geometry table reaches the shader as a push-constant BDA, not a descriptor
Mirroring `skinning.comp`'s `GL_EXT_buffer_reference2` pattern, the table's device address travels in `GiPC` — **zero** ViewResources/pool/Set-2 changes. Read at GI `AddPasses` preflight from the same `m_LastResult` that `GlobalSubsystem` binds to Set 0 b6, so the table's `instanceCustomIndex` mapping can never desync from the bound TLAS.

### `materialUUID` folded into the TLAS rebuild hash
The table carries a per-instance material slot, so a runtime material reassignment on a *static* mesh (no transform/entity/mesh change) must force a rebuild — else GI would shade the secondary hit with the stale material's albedo. `HashInstances` now mixes `materialUUID`'s two halves (review-driven; the raster path re-reads the slot map every frame, which masked it everywhere but GI).

### Skinned secondary hits read bind-pose attributes
The deformed VB is positions-only (12 B, BLAS-only); the original VB (52 B static / 84 B skinned, UVs at +24) is deref'd for UV + geometric normal. The hit *point* rides the deformed TLAS (correct); only n_s/UV are bind-pose — acceptable for a 1-bounce diffuse term, and characters aren't the GI showcase.

### GI SVGF as a channel of the same `SvgfDenoiser`, flat parallel fields
Followed the S0 `restirDI`/`restirGiDI` flat-parallel-field precedent (not a sub-struct) — `svgfGi*` beside `svgf*` — so shipped DI field names + their consumers are untouched. The channel logic is localized to one `Resolve()` + `Settings()` + `PassName()`. The 4 `svgf_*.comp` shaders are channel-agnostic; each instance builds its own pipeline objects.

### Debug-viz scoped to the SVGF-GI toggle + RG snapshot
The plan's "raw / denoised / reservoir M·age" debug toggle: raw-vs-denoised is free via the "SVGF (GI) Enabled" toggle (the denoiser owns Set 3 b6, passthrough-copies raw GI when disabled — no descriptor swap); the GI passes are inspectable via the existing RG snapshot. The dedicated reservoir M·age heat-map (a separate viz pass like ClusterDensity) was deferred as out-of-scope dev-tooling.

---

## Bugs caught (adversarial verification)

- **Geometry-table alloc failure → real TLAS + null-BDA deref** (S3 integration review, **medium**). On host-visible OOM the geom-table buffer is null but the TLAS still built with real instances → a committed hit deref's a null `buffer_reference` → device lost. Fixed: alloc failure aborts the build and returns an empty result (falls back to the persistent empty TLAS → all rays miss).
- **Scratch-alloc-failure buffer leak** (S3 integration review, low). The TLAS-scratch failure early-return leaked the storage + geom-table buffers (their cleanup is keyed on `tlas != null`, but `tlas` was never created). Fixed: free both before the early return (also closed the pre-existing storage-buffer leak on that path).
- **Stale GI material on runtime material swap** (S3 integration review, medium — the `materialUUID` hash fold above).
- **Resolve() DI-branch clobbered** (S4, caught by the build). A broad `replace_all "vr.svgf… → c.…"` over the whole file silently rewrote the DI branch of `Resolve()` itself (it legitimately contains `vr.svgf*`) → `'c' undeclared`. Lesson: scope mechanical field-rename replaces away from the resolver that *defines* the mapping.
- Deferred pre-existing finding (S3 review): `AddTlasBuildPass` is gated on `runRtShadows`, so ReSTIR DI/GI silently no-op under CSM shadow mode (default RtShadows works). Affects shipped DI → spawned as a separate task, not folded into C.3.

---

## Files touched

**Engine — new (S0):** `subsystems/RtRestirGiSubsystem.{h,cpp}`, `settings/RestirGiSettings.h`; shaders `restir_gi_initial/temporal/spatial/shade.comp`, `common/restir_gi_common.glsl` (+ `.meta`).
**Engine — modified:** `backend/vulkan/TlasBuilder.{h,cpp}` (geometry table + `instanceCustomIndex` contract + `materialUUID` hash, S3), `subsystems/RtSubsystem.{h,cpp}` (geom-table lifetime + `GetGeometryTableBDA`, S3), `subsystems/SvgfDenoiser.{h,cpp}` (channel parameterization, S4), `subsystems/LightingSubsystem.cpp` (Set 3 b6 → `svgfGiDenoised`, S4), `subsystems/GlobalSubsystem.cpp` (`restirParams.y` gate, S0), `RenderPipeline.{h,cpp}` + `ViewResources.cpp` (GI subsystem + `m_DenoiseGi` + reservoir/image/pool, S0+S4), `scene/systems/RenderingSystem.h` (`RestirGiSettings` + `SvgfGiSettings`).
**Shaders — modified:** `restir_gi_initial.comp` (S3 real material), `pbr.frag` + `common/globals.glsl` (GI remod, S0).
**Editor:** `panels/RenderPanel.cpp` (ReSTIR GI + SVGF (GI) sections, S5).
**Docs:** `arch/rendering-pipeline.md`.

---

## Verification

Build clean (Debug x64, luth → luthien → Runtime), no new warnings; all new `.comp` pass `glslc --target-env=vulkan1.3`. Four adversarial review workflows (shader-math ×2 in S1/S2, integration ×2 in S3/S4), each multi-skeptic-verified. Smoke-tested per stage under `LUTH_VALIDATION`: S0-S2 converge (temporal+spatial, residual grain is the expected temporal-only floor); S3 true color bleeding (textured surfaces tint the bounce); S4 grain resolved by the GI denoiser with DI unchanged (no cross-contamination); S5 editor sliders functional.

---

## Hand-off / deviations

**Deferred follow-ups (not C.3):** RAY_TRACED bias-correction visibility re-test (near-unbiased; pairs with C.5 path-traced reference); reservoir LogLUV/snorm packing (32-48 B) if VRAM-bound; multi-bounce via temporal feedback; the reservoir M·age debug heat-map (task chip); build-the-TLAS-for-any-RT-consumer fix (task chip — affects shipped DI, needs its own smoke).

**Deviations from the plan:** (1) geometry table via push-constant BDA, not a descriptor binding (zero pool/ViewResources churn). (2) `materialUUID` added to the rebuild hash (not in the plan; review-driven). (3) GI denoiser tuning lives in a separate `SvgfGiSettings` instance (not folded into `RestirGiSettings`). (4) reservoir-field debug viz deferred. (5) Git hooks not installed in this workspace — comment policy honoured manually.
