# v2.1.0 — shader-asset-pipeline

**Date:** 2026-04-18
**Commits:** 5 (on `epic/shader-asset-pipeline`)
**Issue:** [#79](https://github.com/Hekbas/Luth/issues/79)

---

## Overview

First epic of the post-v2.0 architecture-review series. Rewrote the shader asset pipeline around **single-stage shader assets**: each `.vert`, `.frag`, or `.comp` file on disk is one asset with one UUID and one SPIR-V artifact. No more `.vert+.frag` pairing assumption in the importer, no more runtime `ShaderCompiler::Compile` fallback in the renderer, no more `Fragment shader not found` errors on startup.

Minor version bump to **v2.1.0** per the ROADMAP MINOR rule (one completed epic with user-visible changes — gone-startup error, faster launches on second run via cached SPIR-V for all 24 engine shaders).

---

## Sub-Tasks

| # | Sub-task | Commit |
|---|---|---|
| ABC | Single-stage shader asset model (schema + importer + Shader class) | `51d796a refactor(assets): single-stage shader assets (schema, importer, Shader class)` |
| D | RenderPipeline + IBLPrecompute migration through `ShaderLibrary::LoadEngine` | `77f2c2f refactor(renderer): load all engine shaders through asset pipeline` |
| E | Hot-reload for any stage; remove dead `RecompileUtilityShaders` fallback | `de34495 refactor(renderer): hot-reload any shader stage, remove utility-recompile fallback` |
| — | Register `.meta` files for compute shaders (generated on first run) | `d7ab811 chore(assets): register .meta files for compute shaders` |
| F | Docs + v2.1.0 + history + wrap-up | `chore(release): shader-asset-pipeline → v2.1.0` |

(ABC bundled because the schema / importer / Shader-class reshape cannot be split build-clean — see "Sub-task granularity" in the epic spec.)

---

## Schema Change

### Before (v1 shader artifact)

```cpp
struct ShaderAssetData { std::vector<u32> VertexSpirV; std::vector<u32> FragmentSpirV; std::string SourcePath; };
struct ShaderHeader    { u32 VertexSpirVSize; u32 FragmentSpirVSize; };
// Artifact: [AssetHeader(v=1)][ShaderHeader{VertexSize,FragmentSize}][VertSpirV][FragSpirV]
```

`ShaderImporter::Import(.vert)` compiled **both** `.vert` + its paired `.frag`. `.frag` source files had no artifact of their own (importer returned `true` without writing). Vertex-only / fragment-only / compute shaders could not be assets — they were runtime-compiled inline in `RenderPipeline::Initialize` / `RecompileUtilityShaders` / `InitCullPipeline` / `InitAOResources` / `InitDebugBlitResources` / `IBLPrecompute` (≈45 call sites, no caching, every launch).

### After (v2 shader artifact)

```cpp
enum class ShaderStage : u32 { Unknown, Vertex, Fragment, Compute };
struct ShaderAssetData { ShaderStage Stage; std::vector<u32> SpirV; std::string SourcePath; };
struct ShaderHeader    { u32 Stage; u32 SpirVSize; };
// Artifact: [AssetHeader(v=2)][ShaderHeader{Stage,SpirVSize}][SpirV]
```

`DeserializeShader` rejects `version != 2` so V1 artifacts force a re-import under the new schema on first launch after this lands.

---

## Shader / VulkanShader Contract

```cpp
// Shader (was multi-stage container; now single-stage)
class Shader : public Asset {
    virtual ShaderStage GetStage() const = 0;
    virtual const std::vector<u32>& GetSpirV() const = 0;
    virtual const fs::path& GetPath() const = 0;
    virtual bool IsValid() const = 0;
    virtual void Reload() = 0;
    static std::shared_ptr<Shader> Create(ShaderStage, const std::vector<u32>& spirv, const fs::path&);
};

// VulkanShader stores one VkShaderModule + one VkPipelineShaderStageCreateInfo.
// Removed: CompileOrGetVulkanBinaries pairing heuristics.
// Removed: old Create(vertSpv, fragSpv, path) / VulkanShader(vertSpv, fragSpv, path) overloads.
```

Pipeline construction in `RenderPipeline::CreatePipelines` is unchanged — it still consumes raw `m_*Spv` blobs. What changed is where those blobs come from: previously runtime `ShaderCompiler::Compile`, now `ShaderLibrary::LoadEngine("shaders/x.ext")->GetSpirV()` on the shared asset.

---

## ShaderLibrary Changes

- New `ShaderLibrary::LoadEngine(engineRelPath)` — idempotent loader + registrar. Keys library entries by filename (`"pbr.vert"`, `"gtao_main.comp"`). Internally: `AssetDatabase::GetUUID` → `AssetManager::LoadImmediate` → `Register`.
- Keys migrated from friendly names (`"pbr"`, `"shadowDepth"`) to per-stage filenames (`"pbr.vert"`, `"pbr.frag"`, ...).
- Reload callback now handles all 24 engine shader filenames (graphics + compute): pulls fresh SPIR-V into the cached `m_*Spv` blob and rebuilds the affected pipeline(s). Compute-pipeline rebuild is inline (push-constant sizes duplicated from `InitCullPipeline` / `InitAOResources`; acceptable for now).
- File watcher filter extended from `.vert|.frag` to `.vert|.frag|.comp`. Unmatched files now log a warning; the old `m_PendingUtilityReload` fallback flag + `RenderPipeline::RecompileUtilityShaders()` dead-path were deleted.

---

## Scanner Change

`FileSystem::GetAssetTypeFromPath` now recognizes `.comp`. On first launch after this lands, `AssetDatabase::InitEngine` discovers all 8 compute shaders under `luth/assets/shaders/` and generates `.meta` files with stable UUIDs (committed in `d7ab811`). Same dirty-tracking + artifact-cache path as `.vert`/`.frag`.

---

## Call-site Migrations

Replaced every `ShaderCompiler::Compile(shadersPath / "x.ext")` in:

| File | Calls |
|---|---|
| `renderer/RenderPipeline.cpp` (`Initialize`, `InitCullPipeline`, `InitAOResources`, `InitDebugBlitResources`) | 20 |
| `renderer/lighting/IBLPrecompute.cpp` (equirect / irradiance / prefilter / brdf_lut / skybox) | 6 |

`ShaderCompiler::Compile` is now only invoked by (a) `ShaderImporter::Import` (on asset import / hot-reload artifact refresh), and (b) `VulkanShader::Reload` (in-memory recompile of the one stage the shader owns).

Also dropped dead `#include "luth/renderer/shader/ShaderCompiler.h"` from 10 files (9 passes + `RenderingSystem.cpp`) that no longer call it.

---

## Key Design Decisions

### Single-stage asset = disk file
One `.vert` file = one asset = one UUID = one artifact. Pipelines combine stages at creation time, not at import time. This removes the implicit "vert's friend is a frag with the same stem" coupling that broke any vertex-only / compute / fragment-only shader.

### V2 rejects V1 instead of silent upgrade
`DeserializeShader` checks `header.Version != 2` and returns `false`, which triggers a re-import on first load. No silent format conversion, no zombie V1 data on disk. Cleaner than reading both schemas and trying to pick the right one.

### PipelineManager keyed by vertex-shader UUID
`GetOrCreate(shaderUUID, renderMode, cullMode, polyMode, vertSpv, fragSpv)` still uses a single UUID as the cache key. Chose `ShaderLibrary::Get("pbr.vert")->Handle` as the canonical key for the PBR pipeline family (vs. introducing a synthetic program-UUID or hashing both). Stable across launches, consistent with the old behavior (the old `"pbr"` library entry WAS `pbr.vert`). The reload callback invalidates with the canonical key regardless of which stage edited — so a `pbr.frag` reload correctly drops cached PBR pipelines.

### ABC bundled into one commit
Schema rewrite (A), importer rewrite (B), and `Shader`/`VulkanShader` reshape (C) are interlocked: changing one in isolation breaks the build. The epic's issue-level checklist tracks all three, but the commit is one atomic refactor. D, E, F land as separate commits since they're each build-clean on their own.

### RecompileUtilityShaders deleted (no deprecation window)
After D, every engine shader is in `ShaderLibrary`, so the file-watcher always finds a library match and the `m_PendingUtilityReload` fallback never fires. Deleted along with `m_PendingUtilityReload` and the drain branch in `RenderingSystem::Update` rather than leaving dead code. Per-epic principle: no backwards-compat shims.

### Compute pipeline rebuild inline in the callback
Hot-reload of a `.comp` shader rebuilds the matching `VKComputePipeline` inline in the `ShaderLibrary` reload callback (push-constant sizes + descriptor-layout handle copied from the `Init*` call sites). Could factor out a `RebuildComputePipeline(name)` helper — left inline for this epic; clean-up candidate for `render-pipeline-split` (E6 in the review plan).

---

## Build Verification

- 5 work commits on `epic/shader-asset-pipeline`; every commit builds Debug x64 clean (MSBuild `/v:minimal` reports zero errors; only pre-existing warnings — C4267/C4244/C4996/LNK4006).
- Full 3-project solution (`Luth`, `Luthien`, `Runtime`) builds unchanged.
- `grep ShaderCompiler::Compile` — only 2 legitimate call sites remain (`ShaderImporter::Import`, `VulkanShader::Reload`).
- `grep "VertexSpirV\|FragmentSpirV"` — zero in source (only in history docs).

---

## Runtime Verification (user smoke test)

- Delete `luth/Library/Artifacts/` → relaunch: every `.vert`/`.frag`/`.comp` imports once, no `Fragment shader not found` error.
- Second launch: no SPIR-V recompilation (artifact mtimes unchanged).
- Full render: PBR + shadows + GTAO + bloom + skybox + outline + grid + ImGui visually identical.
- Frame Debugger captures + replays a frame.
- Skinned mesh renders with shadows.
- Edit `depthPrepass.vert` → hot-reload fires → depth pass rebuilds.
- Edit `gtao_main.comp` → compute pipeline rebuilds; AO still renders.

---

## Lessons

**Pairing assumptions leak into asset schemas.** The root cause of the `Fragment shader not found` error wasn't the importer's check — it was that the importer had ever been modeled around graphics-pipeline topology in the first place. Fix at the asset-model level, not the importer error path.

**Atomic commits don't always mean one sub-task per commit.** The issue body split the refactor into A/B/C so the scope tracking stays granular on GitHub, but the build constraint forced A+B+C into one commit. The epic spec documents this explicitly so future-me doesn't re-split mid-execution.

**Dead-code cleanup compounds.** Removing `RecompileUtilityShaders` also cleared `m_PendingUtilityReload`, a drain branch in `RenderingSystem::Update`, 9 dead `#include`s in passes, and the `shadersPath` locals that `ShaderCompiler::Compile` calls used to need. Each by itself would be noise; together they materially simplify the surrounding code.

**Startup log noise is a real bug.** The `Fragment shader not found` line was printed on every launch for months. Users (and future me) learn to tune it out, then miss real issues near it. Eliminating it wasn't cosmetic — it was restoring signal.
