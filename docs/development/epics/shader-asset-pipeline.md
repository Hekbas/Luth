# Epic: shader-asset-pipeline

**Issue:** #79  |  **Target:** v2.1.0  |  **Est.:** Medium (1–2 days)  |  **Deps:** none

---

## Goal

Rework the shader asset pipeline so every shader file on disk (`.vert`, `.frag`, `.comp`) is a first-class asset with a cached SPIR-V artifact, hot-reload support, and no `.vert`+`.frag` pairing assumption. Eliminates the startup `Fragment shader not found` error and removes the runtime `ShaderCompiler::Compile()` fallback that bypasses the artifact cache.

---

## Sub-Tasks and Commit Plan

> **Commit granularity note:** Sub-tasks A, B, C on the GitHub issue form one atomic refactor — changing the asset schema in isolation breaks the build, so they land as a single commit. Sub-tasks D–F are independent and get their own commits.

### ABC: Single-stage shader asset model (atomic refactor)

**Commit:** `refactor(assets): single-stage shader assets (schema, importer, Shader class)`
**Trailer:** `Part of #79`
**Issue items:**
- A. Schema + scanner
- B. ShaderImporter rewrite
- C. Shader / VulkanShader single-stage refactor

| File | Change | Notes |
|------|--------|-------|
| `luth/source/luth/resources/importers/ShaderImporter.h` | EDIT | Add `ShaderStage` enum. Reshape `ShaderAssetData` → `{ Stage, SpirV, SourcePath }` |
| `luth/source/luth/resources/importers/ShaderImporter.cpp` | EDIT | Full rewrite — compile one file → write one `ShaderAssetData`. Stage inferred from extension. |
| `luth/source/luth/resources/AssetSerializer.h` | EDIT | `ShaderHeader` → `{ Stage, SpirVSize }` |
| `luth/source/luth/resources/AssetSerializer.cpp` | EDIT | `SerializeShader` writes `header.Version = 2`; `DeserializeShader` rejects `Version != 2` (forces re-import of stale artifacts) |
| `luth/source/luth/resources/AssetManager.cpp` | EDIT | `FinalizeAsset(Shader)` calls `Shader::Create(stage, spirv, sourcePath)` |
| `luth/source/luth/resources/FileSystem.cpp` | EDIT | Add `.comp → AssetType::Shader` to extension map |
| `luth/source/luth/renderer/shader/ShaderCompiler.h/.cpp` | EDIT | Move extension→stage logic into shared `InferStage(path)` helper |
| `luth/source/luth/renderer/shader/Shader.h` | EDIT | Replace paired `Create(vertSpv, fragSpv, path)` with `Create(stage, spirv, path)`. Add `GetStage()` / `GetSpirV()` |
| `luth/source/luth/renderer/backend/vulkan/VulkanShader.h/.cpp` | EDIT | Single-stage ctor, one `VkShaderModule`, one `VkPipelineShaderStageCreateInfo`. Remove `CompileOrGetVulkanBinaries` pairing heuristics. |
| `docs/development/epics/shader-asset-pipeline.md` | NEW | Epic spec (this file) — landed with this commit for progress tracking |

**Verify:**
- [ ] Build succeeds — but pipeline-creation sites in `RenderPipeline.cpp` may not work until task D (they still call `ShaderCompiler::Compile` directly, which is fine for this step).
- [ ] Delete `luth/Library/Artifacts/` — launch engine; every `.vert`, `.frag`, `.comp` in `luth/assets/shaders/` produces an artifact.
- [ ] No `Fragment shader not found` errors in startup log.
- [ ] PBR + shadow shaders (the two already routed through AssetManager) still resolve — engine renders.

---

### D: RenderPipeline + IBLPrecompute migration (LoadEngineShader helper)

**Commit:** `refactor(renderer): load engine shaders through asset pipeline`
**Trailer:** `Part of #79`
**Issue items:**
- D. RenderPipeline + IBLPrecompute migration

| File | Change | Notes |
|------|--------|-------|
| `luth/source/luth/renderer/shader/ShaderLibrary.h/.cpp` | EDIT | Add `LoadEngine(const std::string& relPath)` helper — loads shader asset via AssetManager and registers in the library, returning the shader. |
| `luth/source/luth/renderer/RenderPipeline.cpp` | EDIT | Replace all `ShaderCompiler::Compile(shadersPath / "x.vert")` calls in `Initialize`, `RecompileUtilityShaders`, `InitCullPipeline`, `InitAOResources`, `InitDebugBlitResources` with `ShaderLibrary::LoadEngine("shaders/x.vert")->GetSpirV()`. |
| `luth/source/luth/renderer/lighting/IBLPrecompute.cpp` | EDIT | Same replacement for 6 compile calls (4 compute, 2 skybox) |

**Verify:**
- [ ] Build succeeds, no new warnings
- [ ] Full render smoke: PBR + shadows + GTAO + bloom + skybox + outline + grid + skinned mesh
- [ ] Frame Debugger captures/replays
- [ ] `luth/Library/Artifacts/` contains a `.shader` artifact per file after first launch; second launch reuses artifacts (check mtimes)

---

### E: Hot-reload watcher for any stage

**Commit:** `refactor(renderer): hot-reload any shader stage (incl compute)`
**Trailer:** `Part of #79`
**Issue items:**
- E. Hot-reload watcher

| File | Change | Notes |
|------|--------|-------|
| `luth/source/luth/renderer/RenderPipeline.cpp` | EDIT | Remove the `ext == ".vert" || ext == ".frag"` filter in the `m_System.m_ShaderWatcher.SetCallback`. Accept any of `.vert/.frag/.comp`. Invalidate the matching ShaderLibrary entry + pipelines. |
| `luth/source/luth/scene/systems/RenderingSystem.cpp` | EDIT | If the reload-drain logic has the same filter, align it. |

**Verify:**
- [ ] Edit `depthPrepass.vert` in runtime → hot-reload fires, depth pass rebuilds, no errors
- [ ] Edit `gtao_main.comp` → compute pipeline rebuilds, GTAO renders with any intentional change
- [ ] Edit `fullscreen.vert` → bloom/post chain rebuilds

---

### F: Docs + version + history + wrap-up

**Commit:** `chore(release): shader-asset-pipeline → v2.1.0`
**Trailer:** `Closes #79`
**Issue items:**
- F. Wrap-up

| File | Change | Notes |
|------|--------|-------|
| `luth/source/luth/core/Version.h` (+ .cpp if needed) | EDIT | Bump engine version to `2.1.0` |
| `docs/development/ROADMAP.md` | EDIT | Move `shader-asset-pipeline` to the Completed table |
| `docs/development/ARCHITECTURE.md` | EDIT | Update Shader/Asset-pipeline sections to describe single-stage model |
| `docs/development/history/v2.x/shader-asset-pipeline.md` | NEW | History file — what landed, why, any gotchas |
| `docs/development/epics/shader-asset-pipeline.md` | DELETE | Per workflow — spec is for in-flight epic only |
| `CLAUDE.md` (local, untracked) | EDIT | Update "Current Progress" → v2.1.0, next epic = `math-abstraction` |

**Verify:**
- [ ] Build clean
- [ ] All sub-tasks checked off on GitHub issue
- [ ] End-to-end smoke per `plans/analyze-my-engine-in-magical-moore.md` checklist

---

## Architecture Notes

### Why single-stage?

Coupling `.vert` + `.frag` at the asset layer forces the asset model to encode pipeline topology. Real graphics APIs combine stages at pipeline-creation time, not at import time. Compute shaders, fragment-only shaders (bloomExtract), and vertex-only shaders (depthPrepass) all demand this flexibility. Single-stage assets match the disk representation 1:1 and make extensibility to geometry/tessellation/mesh/raygen trivial.

### New schema

```cpp
// ShaderImporter.h
enum class ShaderStage : u32 {
    Unknown = 0,
    Vertex   = 1,
    Fragment = 2,
    Compute  = 3,
    // Future: Geometry, TessControl, TessEval, Mesh, Task, Raygen, ...
};

struct ShaderAssetData : public AssetData {
    ShaderStage        Stage = ShaderStage::Unknown;
    std::vector<u32>   SpirV;
    std::string        SourcePath;
};

// AssetSerializer.h
struct ShaderHeader {
    u32 Stage;       // ShaderStage
    u32 SpirVSize;   // u32 count
    // Followed by: SpirV [SpirVSize * sizeof(u32)]
};
```

`AssetHeader::Version` for shaders bumps to **2**. Deserializer rejects version 1 and returns failure, triggering re-import.

### Shader class contract

```cpp
// Shader.h
class Shader : public Asset {
public:
    virtual ShaderStage GetStage() const = 0;
    virtual const std::vector<u32>& GetSpirV() const = 0;
    virtual const fs::path& GetPath() const = 0;
    virtual bool IsValid() const = 0;
    virtual void Reload() = 0;

    static std::shared_ptr<Shader> Create(ShaderStage stage, const std::vector<u32>& spirv, const fs::path& path);
    // Reflection (unchanged)
};
```

### Pipeline-creation impact

`RenderPipeline::CreatePipelines` currently consumes `m_*VertSpv` / `m_*FragSpv` raw vectors. Those members stay — they just get populated from the asset pipeline now. No change to `VKGraphicsPipeline` / `VKComputePipeline` APIs.

### ShaderLibrary keying

New convention: register by relative source filename (e.g. `"pbr.vert"`, `"pbr.frag"`, `"gtao_main.comp"`). Existing `"pbr"` / `"shadowDepth"` friendly-name registrations are replaced.

### Hot-reload

The watcher scans all `.vert`/`.frag`/`.comp` files. When one changes, it looks up the corresponding library entry by filename, calls `Shader::Reload()` which re-invokes `ShaderCompiler::Compile` on the one file, then triggers a pipeline-invalidate callback. Pipelines that reference that shader stage get rebuilt in `CreatePipelines`.

---

## References

- `docs/development/BACKLOG.md` — shader asset pipeline (section TBD — may be absent)
- `docs/development/ARCHITECTURE.md` — Renderer/Assets section (to be updated in sub-task F)
- `plans/analyze-my-engine-in-magical-moore.md` — parent review plan, E1
- Prior art: `luth/source/luth/resources/importers/TextureImporter.cpp` (single-stage-like importer pattern)

---

## Progress Tracker

| Sub-Task | Status | Commit | Date |
|----------|--------|--------|------|
| ABC: Single-stage shader asset model | done | 51d796a | 2026-04-18 |
| D: RenderPipeline + IBLPrecompute migration | done | 77f2c2f | 2026-04-18 |
| E: Hot-reload any stage | in-progress | — | — |
| F: Docs + version + history | pending | — | — |
