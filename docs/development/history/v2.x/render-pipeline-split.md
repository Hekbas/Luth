# v2.5.0 — render-pipeline-split

**Date:** 2026-04-19
**Commits:** 8 (on `epic/render-pipeline-split`)
**Issue:** [#83](https://github.com/Hekbas/Luth/issues/83)

---

## Overview

Fifth epic of the post-v2.0 architecture-review series. `RenderPipeline.cpp` — a 3,104-LOC god-orchestrator accumulating init, per-frame update, pipeline-factory and frame-debugger code since v1.7 `arch-renderer-split` — split across 7 sibling topic files and one new `FrameDebuggerContext` class. `RenderPipeline.cpp` shrinks to **781 LOC** (−75%), a thin `Initialize` / `Shutdown` / `Execute` / `OnResize` / `CaptureSnapshot` orchestrator plus shader hot-reload plumbing.

No runtime behavior change. Every init helper and pipeline builder keeps its exact contract and byte-identical body; the only semantic change is the debugger class extraction, which moves 12 preview-texture fields and 8 methods off `RenderPipeline` into a `unique_ptr<FrameDebuggerContext>` member with thin forwarding accessors so external callers (`RenderingSystem`, editor panels) stay unchanged.

Fixes **F5** (`RenderPipeline.cpp` at ~3,150 LOC — ~40% init + debug infrastructure) and **F9** (`CreatePipelines` — single 388-LOC function creating 15+ Vulkan pipelines) from the architecture review. Minor version bump to **v2.5.0** per the ROADMAP rule.

---

## Sub-Tasks

| # | Sub-task | Commit |
|---|---|---|
| A | Extract ShadowInit + IBLInit → `renderer/lighting/` | [`d244e8e`](../../../../commit/d244e8e) |
| B | Extract PostProcessInit → new `renderer/postprocess/` | [`41549ed`](../../../../commit/41549ed) |
| C | Extract AOInit → `renderer/passes/` | [`ef9280b`](../../../../commit/ef9280b) |
| D | Extract GlobalUniforms → `renderer/resources/` | [`570afd0`](../../../../commit/570afd0) |
| E | Extract GPUObjectBuffers → new `renderer/gpu/` | [`4a5d007`](../../../../commit/4a5d007) |
| F | Break `CreatePipelines` into 8 `PipelineFactory` builders | [`fea80df`](../../../../commit/fea80df) |
| G | Extract `FrameDebuggerContext` class → new `renderer/debug/` | [`2208dc1`](../../../../commit/2208dc1) |
| H | Docs + v2.5.0 + history | this commit |

Seven refactor commits ordered by risk (trivial moves → factory reshape → class extraction). Each builds Debug x64 clean. Smoke test ran after G and before H.

---

## Why split now

By v2.4.0 the engine had finished four architecture-review epics (shader pipeline, math facade, core reorg, animation dissolve) but `RenderPipeline.cpp` still held:

- 5 `Init*Resources` helpers (shadow / post-process / IBL / AO / GPU buffers)
- 3 `Update*` helpers (post-process descriptors + UBO, AO descriptors + GTAO UBO, global uniforms)
- A monolithic 388-LOC `CreatePipelines` that inlined 15 graphics pipelines from 9 different stage buckets
- 8 frame-debugger methods + 12 preview-texture fields, including a 283-LOC `ReplayPassUpToDraw` implementing per-draw replay-then-copy
- Plus the orchestrator itself (`Initialize` / `Shutdown` / `OnResize` / `Execute` / `CaptureSnapshot`) and the 94-LOC shader hot-reload callback

Navigation was "where in the 3,104-LOC file is this?" rather than "what file owns this?". Extracting sub-concerns into peer files lets each file answer a single "what does this set up and why?" question.

---

## Target layout

```
luth/source/luth/renderer/
├── RenderPipeline.{h,cpp}          ← ~780-LOC orchestrator (was 3,104)
├── passes/                          (existing)
│   ├── AOInit.cpp                  ← InitAOResources, UpdateAODescriptors, UpdateGTAOUBO  [C]
│   └── … (existing 12 pass files)
├── lighting/                        (existing)
│   ├── ShadowInit.cpp              ← InitShadowResources                                [A]
│   ├── IBLInit.cpp                 ← InitIBLResources, ReloadSkybox                     [A]
│   └── … (existing LightGatherer, CascadeBuilder, IBLPrecompute, LightTypes)
├── postprocess/                     NEW
│   └── PostProcessInit.cpp         ← InitPostProcessResources + PP descriptors + PP UBO [B]
├── resources/                       (existing)
│   ├── GlobalUniforms.cpp          ← InitGlobalUniforms, UpdateGlobalUniforms           [D]
│   └── … (existing Texture, Mesh, Model, Buffer, Skeleton, AnimationClip, BoneMatrixBuffer)
├── gpu/                             NEW
│   └── GPUObjectBuffers.cpp        ← 6 object-SSBO + cull + material + light UBO funcs  [E]
├── pipeline/                        (existing)
│   ├── PipelineFactory.cpp         ← 8 per-family builders (PBR/Shadow/DepthPrepass/…)  [F]
│   └── PipelineManager.{h,cpp}     (existing)
└── debug/                           NEW
    ├── FrameDebuggerContext.h      ← 12 preview fields + 8 debugger methods             [G]
    └── FrameDebuggerContext.cpp
```

Premake `files { "source/**.{h,cpp}" }` — recursive glob picks up new folders automatically. Zero `premake5.lua` edits.

---

## `PipelineFactory` — 8 per-family builders (sub-task F)

`CreatePipelines` kept as orchestrator for the two callers (`RenderPipeline::Initialize` and `ReloadSkybox`), body decomposed into named helpers — one per pipeline family:

| Builder | Creates |
|---|---|
| `BuildPBRPipelines` | `m_GeoPipelineManager` + `m_GeoSkinnedPipelineManager` (lazy, keyed by shader+renderMode+cullMode+polygonMode) |
| `BuildShadowPipelines` | `m_ShadowPipeline` + `m_ShadowSkinnedPipeline` (depth-only, front-face cull) |
| `BuildDepthPrepassPipelines` | `m_DepthPrepassPipeline` + `m_DepthPrepassSkinnedPipeline` |
| `BuildSelectionPipelines` | `m_SelectionMaskPipeline` + `m_SelectionMaskSkinnedPipeline` |
| `BuildSkyboxPipeline` | `m_SkyboxPipeline` (depth test + front-face cull to show inside faces after Y-flip) |
| `BuildPostPipelines` | `m_BloomExtractPipeline` + `m_BloomBlurPipeline` + `m_PostProcessPipeline` |
| `BuildOutlinePipeline` | `m_OutlinePipeline` |
| `BuildGridPipeline` | `m_GridPipeline` |

Three file-local anonymous-namespace helpers (`MakePBRVertexLayout`, `MakeSkinnedVertexLayout`, `MakePositionOnlyWithFullStride`) deduplicate the three distinct vertex layouts shared across the rigid/skinned/depth-only pipelines. Nothing changes about descriptor-set wiring or push-constant ranges — each builder reconstructs its own `geoLayouts` / `layouts` vector from member-resident descriptor-layout handles.

---

## `FrameDebuggerContext` — the only new class (sub-task G)

Of all the splits, only the frame debugger benefits from a true class boundary. The 12 preview-texture fields + 8 methods form coherent render-side infrastructure (preview image allocation/teardown, debug blit pass, per-draw replay, depth-archive blit) that cleanly separates from orchestration.

### Shape

```cpp
// renderer/debug/FrameDebuggerContext.h
class FrameDebuggerContext
{
public:
    explicit FrameDebuggerContext(RenderPipeline& pipeline);
    ~FrameDebuggerContext();

    void Shutdown();
    void InitDebugBlitResources();
    RG::ResourceHandle AddDebugBlitPass(RG::RenderGraph&, RG::ResourceHandle, bool isDepth);
    void ReplayPassUpToDraw(u32 passIdx, u32 localDrawIdx);
    void BlitArchivedDepthToPreview(u32 archiveIdx, int layer, float nearZ, float farZ);

    // Preview-texture accessors (forwarded from RenderPipeline)
    VkImageView GetPerDrawPreviewView()  const;
    // … 6 more getters + ResetPreviewCacheKeys

private:
    void EnsurePerDrawPreviewTexture(u32 w, u32 h);
    void DestroyPerDrawPreviewTexture();
    void EnsureDepthPreviewTexture(u32 w, u32 h);
    void DestroyDepthPreviewTexture();

    RenderPipeline& m_Pipeline;
    // 12 preview-texture fields (image/view/alloc/width/height/key × 2)
};
```

### Wiring

- `RenderPipeline` holds `std::unique_ptr<FrameDebuggerContext> m_Debugger`. Forward-declared in the header so `FrameDebuggerContext.h` stays out of the hot include path; destructor defined non-inline in `.cpp` where the class is complete (required by `unique_ptr` default-deleter).
- Public accessors (`GetPerDrawPreviewView`/`GetDepthPreviewWidth`/`ResetPreviewCacheKeys`/`ReplayPassUpToDraw`/`BlitArchivedDepthToPreview`) stay on `RenderPipeline` as thin forwarders that call `m_Debugger->*`. External callers — `RenderingSystem::GetPerDrawPreviewView`, `FrameDebuggerPanel::DrawLiveView`, `ProfilerPanel::GetGraphSnapshot` — compile unchanged.
- Friendship: both `RenderPipeline` and `RenderingSystem` declare `friend class FrameDebuggerContext`, matching the existing `RenderingSystem::friend RenderPipeline` pattern. This lets `FrameDebuggerContext` reach `m_Pipeline.m_System.m_FrameDebugger` (blit pipeline, depth pipeline, descriptor set) and `m_Pipeline.m_GeoPipelineManager` / `m_PBRVertSpv` / `m_IndirectBuffer` without a wide public-accessor surface.

### Naming disambiguation

`Luth::FrameDebugger` (owned by `RenderingSystem`) — archive + state machine + captured-frame metadata.
`Luth::FrameDebuggerContext` (owned by `RenderPipeline`) — render-side Vulkan infrastructure (preview textures, blit pass, per-draw replay). Distinct concerns, adjacent files eventually once E7 `rendering-system-slim` moves `FrameDebugger` out of `RenderingSystem`.

---

## Migration mechanics

### Perl-based body extraction

Each sub-task used a single `perl -i -0pe` pass to delete a function body from `RenderPipeline.cpp` after copying it verbatim into the new file:

```perl
perl -i -0pe 's/\r?\n    void RenderPipeline::InitShadowResources\(\)\r?\n    \{.*?\r?\n    \}\r?\n//s' RenderPipeline.cpp
```

Non-greedy `.*?` with `/s` matches the shortest body up to the function-level `\n    }\n` (4-space indent distinguishes function close from nested scopes). CRLF preserved (`\r?\n` in regex; perl `-i` writes bytes, not text). Orphaned section banners deleted in the same pass when their last function left the file.

### Header updates

- `RenderPipeline.h`: forward-declare `FrameDebuggerContext`; add `friend class FrameDebuggerContext`, `~RenderPipeline()`, `unique_ptr<FrameDebuggerContext> m_Debugger`, 8 `BuildXxxPipelines` declarations; remove 6 debugger-method declarations + 12 preview-texture fields; change 7 inline preview accessors to out-of-line declarations (body can't dereference forward-decl'd unique_ptr).
- `RenderingSystem.h`: add `friend class FrameDebuggerContext` next to existing `friend class RenderPipeline`.
- `RenderPipeline.cpp`: add `#include "luth/renderer/debug/FrameDebuggerContext.h"`; construct `m_Debugger` in ctor; `~RenderPipeline() = default;`; `Execute`'s capture branch uses `m_Debugger->InitDebugBlitResources()` + `m_Debugger->ResetPreviewCacheKeys()`; `Shutdown` calls `m_Debugger->Shutdown()` before Vulkan teardown; add 10 thin forwarder implementations.

---

## Final tally

| Metric | Before | After |
|---|---:|---:|
| `RenderPipeline.cpp` LOC | 3,104 | 781 |
| `CreatePipelines()` monolith | 388 LOC, one function | 9 functions in `PipelineFactory.cpp` (orchestrator + 8 builders) |
| Frame-debugger methods on `RenderPipeline` | 8 public + 12 fields | 2 public forwarders + `unique_ptr<FrameDebuggerContext>` |
| `renderer/` sub-folders | 10 | 13 (+`postprocess/`, `gpu/`, `debug/`) |
| Refactor commits | — | 7 (each builds Debug x64 clean) |
| External caller changes | — | 0 (public API unchanged via forwarders) |

---

## Build Verification

- 7 refactor commits on `epic/render-pipeline-split`; every commit builds Debug x64 clean with no new warnings.
- Only pre-existing warnings (LNK4006 from `vulkan-1.lib`, C4267 size_t narrowing in MSVC `<xutility>`, C4996 `getenv`/`strncpy` in editor, C4244 chrono narrowing in `Editor.cpp:420`).
- `rg "RenderPipeline::(InitShadow|InitIBL|ReloadSkybox|InitPostProcess|UpdatePostProcessDescriptors|UpdatePostProcessUBO|InitAO|UpdateAO|UpdateGTAO|InitGlobalUniforms|UpdateGlobalUniforms|InitObjectSSBODescriptorLayout|InitGPUObjectBuffers|InitCullPipeline|EnsureMaterialRegistered|BuildGPUObjectBuffer|UploadLightUBO|InitDebugBlitResources|AddDebugBlitPass|EnsurePerDrawPreviewTexture|DestroyPerDrawPreviewTexture|EnsureDepthPreviewTexture|DestroyDepthPreviewTexture)" RenderPipeline.cpp` returns 0 matches — all 23 function bodies moved out.
- Runtime smoke (user-tested): PBR + shadows + GTAO + bloom + skybox + outline + grid + ImGui render identically; Frame Debugger captures + replays a frame with per-draw preview + cascade depth preview; skinned mesh animates + casts shadows; mouse picking selects entities; shader hot-reload fires and rebuilds pipelines live.

---

## Lessons

**File-local helpers are the right amount of dedup for self-contained builders.** `PipelineFactory.cpp`'s three anonymous-namespace helpers (`MakePBRVertexLayout`, `MakeSkinnedVertexLayout`, `MakePositionOnlyWithFullStride`) dedupe the three distinct vertex layouts shared across the eight builders. Class-level helpers would have made the layouts usable by callers outside the factory — premature. Repeating the seven-line `geoLayouts = {...}` vector in each builder that uses it is the honest trade-off: each builder reads start-to-finish without chasing helpers.

**Forwarding is the cheap way to refactor a class without breaking callers.** `FrameDebuggerContext` has 10 entry points (8 methods + 7 getters + `ResetPreviewCacheKeys`), called from 5 external sites across `RenderingSystem`, `FrameDebuggerPanel`, and `ProfilerPanel`. Moving those methods into the new class and leaving a thin forwarder on `RenderPipeline` shrinks the diff to "new class + a handful of 1-line methods" instead of touching every caller. When the public surface is stable, friendship + forwarders is the lower-risk path vs. rewriting callsites.

**A "just move the body" commit can still be large.** Sub-task G (FrameDebuggerContext extraction) touched 5 files and 1,603 changed lines in one commit — large, but not splittable without half-state. The class must exist, its friendships must be in place, and the RenderPipeline forwarders must dispatch to it all at once, otherwise the build breaks mid-commit. Sub-tasks A-F were splittable because each was a pure function-body move; G was a structural change and belonged in one atomic commit.
