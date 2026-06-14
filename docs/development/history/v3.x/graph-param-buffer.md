# graph-param-buffer (v3.2.6)

**Date:** 2026-06-14 · **Issue:** Part of [#157](https://github.com/Hekbas/Luth/issues/157) · **Series:** `slang-material` (constants→data follow-up) · **Branch:** `feat/graph-param-buffer`

## Summary

The node-graph material editor (v3.2.5) baked graph constants (`ConstFloat`/`ConstColor`/`Remap` values) as **literals in the generated Slang**. That coupled value to structure and caused four linked problems: a value edit regenerated + recompiled the shader (and transitively reloaded ~6 RT megakernels → an authoring hitch); every distinct material was a distinct RT variant (hard-capped at 16); each codegen minted a new `Shader` handle → new raster pipeline + an ever-growing `m_GraphFragSpv` cache; and — the root — constants were code, not data.

This effort routes graph constants through **per-material data** and keys variants by **graph structure**. A value edit is now pure data (no shader regen); structurally-identical materials (same topology + node types + tex slots, differing only in values) **share one compiled shader + one RT variant**, their constants living in a per-material buffer. The recompile hitch, the 16-cap, and the per-edit leak dissolve together.

This is the **industry-standard model**, validated against primary sources before implementing: Unreal Material Instances (scalar/vector parameters reuse the parent's compiled shader as uniform data — only *static switch* parameters force a permutation), Unity's SRP Batcher (*"as many materials with the same shader as you want"* + a per-material `UnityPerMaterial` buffer resident on the GPU), and Unity Shader Graph (exposed properties = data vs keywords = variants). The baked-literal approach was the documented anti-pattern (equivalent to making every value a static switch). Our per-material param buffer indexed by material slot mirrors Unreal GPUScene / Unity's persistent per-material buffer — and Luth's own material SSBO.

## What shipped

### 1. Per-material graph-params buffer (`gMatParams`)

A second per-frame buffer parallel to the material SSBO: fixed `MAT_GRAPH_STRIDE` (16) `float4` per material slot, indexed by the same slot. Allocated each frame in `MaterialSystem::Update` from `GPUTaggedPageAllocator` (same tag / `FlushRegion` / `FreeTag(N-2)` lifecycle as the SSBO — composes with the tagged heap, no fixed pool, no VMA-direct). Bound as **binding 1 on MaterialSystem's existing descriptor set layout**: because that one cycled set is bound at **Set 2 for every raster pipeline and Set 3 for every RT megakernel**, a single layout edit gives both tiers the param buffer with zero subsystem changes. Zero `GPUMaterialData` change → `MaterialLayoutGuard` untouched.

### 2. The two-tier param seam

`ITexFetch` gains `float4 Param(uint slot)`; `RasterFetch` / `RayFetch` carry a `uint paramBase` and read `gMatParams[paramBase + slot]`. Consumers set `paramBase = materialIndex * MAT_GRAPH_STRIDE` (raster: `i.materialIndex`; RT: `g.materialSlot`). The generated `EvalGraph<F>` body emits `fetch.Param(k)` with `k` the node's canonical slot. `Remap`'s affine moved off baked scale/bias into a runtime `RemapApply(v, p)` over the raw `(inMin,inMax,outMin,outMax)` param — mandatory, else the value would re-leak into the structure.

### 3. Canonicalized codegen

The Lowerer was rewritten to name SSA locals and assign param slots by **DFS visit-order index** (`CanonicalOrder`), not editor-assigned node IDs — including the Split mid-expression use-site. The emitted module source is therefore a pure function of structure (value-free, ID-free). A shared `BuildParams` extractor walks the *same* `CanonicalOrder` + `IsValueNode` predicate, so `m_GraphParams[k]` lines up with the generated `fetch.Param(k)` by construction. A hard `paramCount ≤ MAT_GRAPH_STRIDE` guard fails loud (material renders stock) rather than letting `fetch.Param(k)` read into the next material's region.

### 4. Per-material param cache + upload

`Material` caches `std::vector<Vec4> m_GraphParams`, rebuilt off the frame loop on a value/structure edit (`GenerateAndCompile` / new `MaterialGraphCodegen::RefreshParams`). `MaterialSystem::Update` just `memcpy`s the cache into each graph material's param block every frame (the region is fresh each frame; non-graph slots stay untouched) and writes both bindings unconditionally (binding 1 is never left unbound under UAB).

### 5. Editor: value edits become data

`MaterialGraphPanel::DrawNodeParams` now returns `{value, structure}`: a `Const`/`Remap` value settle → `RefreshParams` + `MarkDirty` (no codegen); a `TextureSample` slot change (and add/delete/link) → `GenerateAndCompile`. The recompile-on-every-value-drag hitch is gone.

### 6. Structure-keyed variants

`s_Variants` (material-UUID → variant) became `s_Structures` (FNV-1a hash of the canonical source → `{variant, shaderUUID, canonSrc}`). On `GenerateAndCompile`: hash the canonical source; on a **hit** (with a `memcmp` source confirm against collision) reuse the shader + variant and skip compile / registry / RT reload entirely; on a **miss** compile once, assign a variant, store, regenerate the registry, and `ScheduleRtReload`. Generated module / consumer / registry entries are keyed by structure hash, so the raster pipeline (cached per `fragShaderUUID`) and the RT switch arm are both shared per structure. The 16-cap now counts distinct **structures**, not materials.

## Design decisions / deviations

- **Full structure-keying, not the minimal "constants→data, keep per-material variants."** The stable per-material param-slot assignment that the minimal scope needs *is* the canonicalization the full scope needs, so minimal would have done ~80 % of the work while leaving the 16-cap, the per-structure duplicate compiles, and half the `m_GraphFragSpv` cache leak alive. Landed as two commits (constants→data, then structure-keying) for a bisect checkpoint.
- **Fixed-stride-by-material-slot param buffer, not a compacted graph-param slot.** Indexing per-material data by slot into one big buffer is exactly how Unity's SRP Batcher (`UnityPerMaterial`) and Unreal's GPUScene (`GetPrimitiveData(PrimitiveId)`) store it, and it mirrors Luth's own material SSBO (also fixed-stride-by-slot). It introduces no new cap (aligning with "remove the caps") and no free-list. The cost is ~4 MB/frame regardless of graph-material count — the same shape as the 1.25 MB material SSBO, accepted for the same reason.
- **Structure hash over the canonical *generated source*, not the graph struct.** Hashing the emitted (value-free, ID-free) module string automatically folds in everything the compiler actually sees (node types, tex slots, topology, default-input fallbacks) and excludes nothing relevant. Dead nodes are DFS-stripped before hashing, so a disconnected node can't split an otherwise-shared structure. A stored-source `memcmp` on a hash hit makes a 64-bit collision a non-issue.
- **Implicit structural dedup vs Unreal's explicit parent/instance.** Unreal requires an explicit Material → Material Instance link to share a shader; we auto-dedup by structure hash, so cloning a material and tweaking values shares the shader with no setup. Since only the *compiled shader* is shared (values stay independent in the param buffer), there is no user-visible coupling — it is a pure optimization.

## Bugs fixed along the way

- **Slang 41016 "use of uninitialized variable 'rf'."** Adding a data member (`paramBase`) to `RasterFetch`/`RayFetch` made `RasterFetch rf;` trip Slang's uninitialized-variable analysis in the stock decode paths (which never set `paramBase`); a default member initializer (`= 0`) did not suppress it. Fixed with an explicit `__init() { paramBase = 0; }` on both fetch structs — confirmed clean via offline `slangc` on the stock + generated + RT paths.

## Files

- **Engine:** `assets/shaders/common/material.slang` (`MAT_GRAPH_STRIDE`, `ITexFetch::Param`, `RemapApply`), `common/material_bindings_raster.slang` + `common/material_bindings_rt.slang` (`gMatParams`, `paramBase`, `__init`), `renderer/material/Material.{h,cpp}` (`m_GraphParams` cache + accessors; flags-bits comment fix), `renderer/material/MaterialGraphCodegen.{h,cpp}` (canonicalization, `BuildParams`, `RefreshParams`, structure hash + `s_Structures`), `renderer/material/MaterialSystem.cpp` (binding 1 layout/pool + param region upload), `core/Version.h`
- **Editor:** `luthien/panels/MaterialGraphPanel.cpp` (value-vs-structure split)
- **Docs:** `ROADMAP.md`

## Sub-tasks (commit order)

| # | Commit | Notes |
|---|--------|-------|
| C1 | `feat(material): graph constants through per-material data` | `gMatParams` seam + binding 1 + canonicalized codegen (`fetch.Param`) + `RemapApply` + per-material upload + editor value-as-data; variants stay per-UUID |
| C2 | `feat(material): structure-keyed graph shader variants` | FNV-1a structure hash + `s_Structures` + hash-hit reuse (skip compile/registry/reload); cap counts structures |

## Verification

- Each commit built Debug x64 clean — no new warnings (only the pre-existing C4996 `getenv`/`strncpy`, C4244 chrono, C4267 size-narrowing, and the `vulkan-1.lib` LNK4006).
- Both tiers offline-`slangc`-validated against the engine's session flags (`spirv_1_5` / `fp-mode precise` / column-major): a hand-authored sample of the generated raster module + consumer (`fetch.Param` + `RemapApply` + `paramBase`), the stock `pbr.slang` seam, and a compute driver exercising the full RT decode (`RayFetch` → `EvalGraphVariant` via a registry override → `EvalGraph`) all emit SPIR-V.
- **Runtime smoke (user):** a graph material renders its routing in the viewport and identically in Path-Trace (raster == RT); a `Const`/`Remap` value drag updates the look with **no recompile log**; two materials cloned from one graph with different values share a pipeline + variant and both edit instantly; non-graph materials are byte-identical; `SlangParityGuard` stays green (variant 0 = stock decode).
