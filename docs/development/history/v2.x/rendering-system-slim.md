# v2.6.0 — rendering-system-slim

**Date:** 2026-04-19
**Commits:** 4 (on `epic/rendering-system-slim`)
**Issue:** [#84](https://github.com/Hekbas/Luth/issues/84)

---

## Overview

Final epic of the post-v2.0 architecture-review series. `scene/systems/RenderingSystem` — labelled "ECS glue layer" by its own doc-comment since `arch-renderer-split` (v1.7.0) but actually driving five unrelated concerns — sheds three of them into dedicated systems / services. `RenderingSystem` drops from **533 LOC (194 h + 339 cpp)** to **385 LOC (169 h + 216 cpp)**, −28%, and its `.cpp` include list shrinks from 30 to 15. Sibling systems (`CameraSystem`, `TransformSystem`, `AnimationSystem`) average 50–100 LOC; `RenderingSystem` is no longer an order-of-magnitude outlier.

No runtime behavior change. Extractions are pure file/ownership moves: `LightGatherer` + `CascadeBuilder` gain a lean owner (`LightingSystem`) instead of sitting loose on `RenderingSystem`; the `FileWatcher` + reload-queue plumbing becomes a proper `ShaderWatcher` service owned by `RenderPipeline` (where its callback logic already lived); the 59-line inline Vulkan picking-readback moves onto `PickingSystem` where it naturally belongs.

Fixes **F6** (RenderingSystem owns lighting gather + cascade build + picking + shader hot-reload + render orchestration) from the architecture review. Minor version bump to **v2.6.0** per the ROADMAP rule.

---

## Sub-Tasks

| # | Sub-task | Commit |
|---|---|---|
| A | Extract `LightingSystem` → `scene/systems/` (owns `LightGatherer` + `CascadeBuilder` + outputs) | [`a69e696`](../../../../commit/a69e696) |
| B | Extract `ShaderWatcher` service → new `renderer/shader/` file; owned by `RenderPipeline` | [`a90d834`](../../../../commit/a90d834) |
| C | Extract `PickingSystem` → `scene/systems/` (owns 59-line Vulkan readback) | [`012d951`](../../../../commit/012d951) |
| D | Trim `RenderingSystem` includes + docs + v2.6.0 + history | this commit |

Three atomic refactor commits ordered from lowest-coupling to highest-coupling extraction: lighting (stateless helpers, simple ownership move); shader watcher (already-split callback, consolidates state); picking (spans engine + editor, needs App-level registration + ScenePanel rewire). Each builds Debug x64 clean. Smoke test ran before D.

---

## Why slim now

After E6 (`render-pipeline-split` → v2.5.0) `RenderPipeline.cpp` became a lean orchestrator. The remaining responsibility-shedding target was `RenderingSystem`, which by its own header comment claimed "ECS-glue layer" but carried:

- Lighting CPU pipeline — `LightGatherer` + `CascadeBuilder` instances, `DirectionalLightShadowParams` + `CascadeData` + `LightUniforms` members, `UpdateLightUniforms()` private driver
- Mouse picking — `m_PickPending` / `m_PickResultReady` / `m_PickCoord` / `m_PickedEntity` state plus a 59-line inline Vulkan staging-buffer readback in `Update()`
- Shader hot-reload — `FileWatcher m_ShaderWatcher`, `m_WatchedProjectShaderDir`, `m_ReloadMutex`, `m_PendingReloads`, pending-queue drain loop, `OnProjectLoaded/Unloaded` watcher mutation — with the actual callback lambda living on `RenderPipeline::Initialize` and reaching back through `friend` to enqueue into `m_System.m_PendingReloads`
- Per-frame orchestration — material registration → `MaterialSystem::Update` → `BuildGPUObjectBuffer` → `DrawListBuilder::Build` → `Pipeline::Execute`
- Frame-debugger pass-through — ~10 getters delegating to `RenderPipeline`

Only the last two are "ECS-glue". The other three are independent concerns that happened to share a container.

---

## `LightingSystem` (sub-task A)

### Shape

```cpp
// scene/systems/LightingSystem.h
class LightingSystem : public ISystem
{
public:
    void Update(Scene* scene) override {}    // no-op; RenderingSystem drives it

    void UpdateFor(entt::registry& reg, const CameraParams& cam);

    const LightUniforms&                GetLights()       const { return m_Lights; }
    const CascadeData&                  GetCascades()     const { return m_Cascades; }
    const DirectionalLightShadowParams& GetShadowParams() const { return m_Shadow; }

private:
    LightGatherer                m_Gatherer;
    CascadeBuilder               m_Builder;
    LightUniforms                m_Lights{};
    CascadeData                  m_Cascades{};
    DirectionalLightShadowParams m_Shadow{};
};
```

### Wiring

- Registered in `SystemRegistry::Init` between `CameraSystem` and `RenderingSystem`, but its `ISystem::Update` is intentionally empty. CPU ordering (gather → cascade fit → UBO upload → global uniforms) requires lighting to run *inside* `RenderingSystem::Update`, not as an independent SystemRegistry step. Keeping it `ISystem`-shaped makes it reachable via `SystemRegistry::GetSystem<LightingSystem>()` so no new plumbing is needed.
- `RenderingSystem::Update` calls `lighting->UpdateFor(registry, m_CameraParams)`, then hands outputs to `m_Pipeline->UploadLightUBO(...)` + `UpdateGlobalUniforms(cascades, shadow)`.

### Friend-class narrowing

`RenderPipeline::UpdateGlobalUniforms` previously read `m_System.m_Cascades` + `m_System.m_ShadowParams` through `friend class RenderPipeline`. After A, those members are gone from `RenderingSystem`; the signature becomes:

```cpp
void UpdateGlobalUniforms(const CascadeData& cascades, const DirectionalLightShadowParams& shadowParams);
```

Pipeline caches the values into private `m_FrameCascades` / `m_FrameShadowParams` snapshots so `Execute()` (cascade-frustum cull at `RenderPipeline.cpp:437`) and `CaptureSnapshot()` (capturedFrame writes at `RenderPipeline.cpp:571-576`) keep working without further signature churn. **One leg of `friend class RenderPipeline` eliminated**; the remaining friend reads (`DrawList`, `CameraParams`, `FrameTargets`, `PostProcessSettings`, `FrameDebugger`) are all for state `RenderingSystem` legitimately owns and are flagged for a future `pipeline-frame-context` epic.

---

## `ShaderWatcher` (sub-task B)

### Shape

```cpp
// renderer/shader/ShaderWatcher.h
class ShaderWatcher
{
public:
    void Start(const std::filesystem::path& engineShadersDir);
    void Stop();
    void AddProjectDir(const std::filesystem::path& projectShadersDir);
    void RemoveProjectDir();
    void Poll();     // main-thread drain into ShaderLibrary::Reload

private:
    void Enqueue(const std::string& shaderName);

    FileWatcher           m_Watcher;
    std::filesystem::path m_ProjectDir;
    std::mutex            m_Mutex;
    std::set<std::string> m_Pending;
};
```

### Why it wasn't already a class

Before B, the watcher was split awkwardly across two owners:

- **State** (FileWatcher, mutex, pending queue, project dir) lived on `RenderingSystem`.
- **Callback** (ext filter, library lookup, enqueue) was a lambda inside `RenderPipeline::Initialize`.
- **Lifecycle** (`Start(true)` + `Stop`) lived in `RenderPipeline::Initialize` / `::Shutdown` but reached the watcher via `m_System.m_ShaderWatcher.*` friend access.
- **Drain** (iterate pending, call `ShaderLibrary::Reload`) lived at the top of `RenderingSystem::Update`.

Three places, two owners, one underlying responsibility. `ShaderWatcher` collapses it all into one file.

### Wiring

- `RenderPipeline` owns `ShaderWatcher m_ShaderWatcher` as a plain (not `ISystem`) member. `Initialize` calls `m_ShaderWatcher.Start(engineShadersDir)`; `Shutdown` calls `Stop()`.
- `RenderPipeline::Execute` prologue calls `m_ShaderWatcher.Poll()` — replaces the mutex-guarded drain loop that was at the top of `RenderingSystem::Update`.
- `RenderingSystem::OnProjectLoaded/Unloaded` becomes a one-line forward to `m_Pipeline->GetShaderWatcher().AddProjectDir(...)` / `RemoveProjectDir()`. These methods stay on `RenderingSystem` because `App.cpp:317, 342` calls them on the rendering system directly; a larger re-plumbing is out of scope.

---

## `PickingSystem` (sub-task C)

### Shape

```cpp
// scene/systems/PickingSystem.h
class PickingSystem : public ISystem
{
public:
    void Update(Scene* scene) override;    // drains pending readback after render

    void RequestPick(int x, int y);
    bool HasResult() const { return m_Ready; }
    entt::entity ConsumeResult();

private:
    bool         m_Pending = false;
    bool         m_Ready   = false;
    IVec2        m_Coord   = { 0, 0 };
    entt::entity m_Picked  = entt::null;
};
```

### Vulkan readback relocation

The 59-line inline staging-buffer copy (create `VkBuffer` with `VMA_MEMORY_USAGE_GPU_TO_CPU`, transition `EntityID` image to `TRANSFER_SRC_OPTIMAL`, `vkCmdCopyImageToBuffer`, map + read + free) moves verbatim from `RenderingSystem::Update` into `PickingSystem::Update`. No barrier or timing change — the target is `FrameTargets::GetEntityIDBuffer()` which `GeometryPass` always writes; the readback continues to use `VulkanContext::Get().ImmediateSubmit` which synchronously submits + waits.

### Ownership via `SystemRegistry` lookup

`PickingSystem` reaches `FrameTargets*` + `RenderPipeline*` (for `GetEntityLookup()`) at Update time via `SystemRegistry::GetSystem<RenderingSystem>()` plus two new public accessors (`GetFrameTargets`, `GetPipeline`) on `RenderingSystem`. No constructor-injection or ownership-transfer — `PickingSystem` has no members pointing into `RenderingSystem`, so it remains standalone.

### Update order

`App.cpp:246-249` now dispatches:

```
SystemRegistry::Update<TransformSystem>();
SystemRegistry::Update<AnimationSystem>();
SystemRegistry::Update<RenderingSystem>();
SystemRegistry::Update<PickingSystem>();   // after render so EntityID target is fresh
```

### Editor rewire

`ScenePanel.cpp:320-387` switches its 4 call sites from `m_RenderingSystem->RequestPick/HasPickResult/ConsumePickResult` to `SystemRegistry::GetSystem<PickingSystem>()->RequestPick/HasResult/ConsumeResult`. Ctor signature unchanged — matches how `FrameDebuggerPanel` / `RenderPanel` / `ProfilerPanel` already reach `RenderingSystem` via the registry.

---

## Final tally

| Metric | Before | After |
|---|---:|---:|
| `RenderingSystem.h` LOC | 194 | 169 |
| `RenderingSystem.cpp` LOC | 339 | 216 |
| `RenderingSystem` combined LOC | 533 | 385 |
| `.cpp` includes on `RenderingSystem` | 30 | 15 |
| Disparate responsibilities on `RenderingSystem` | 5 | 2 (orchestrate + frame-debugger pass-through) |
| Friend-class reads (`m_System.*` in renderer/) | 9 | 7 (cascade leg eliminated) |
| `scene/systems/` files | 5 | 7 (+LightingSystem, +PickingSystem) |
| `renderer/shader/` files | 5 | 7 (+ShaderWatcher.h/.cpp) |
| Refactor commits | — | 3 (each builds Debug x64 clean) |
| Editor caller changes | — | 1 file (`ScenePanel.cpp`) |

---

## Build Verification

- 3 refactor commits on `epic/rendering-system-slim`; every commit builds Debug x64 clean with no new warnings.
- Only pre-existing warnings (LNK4006 from `vulkan-1.lib`, C4267 size_t narrowing in MSVC `<xutility>`, C4996 `getenv`/`strncpy` in editor, C4244 chrono narrowing in `Editor.cpp:420`).
- `rg "m_System\.m_Cascades|m_System\.m_ShadowParams|m_System\.m_LightGatherer|m_System\.m_CascadeBuilder|m_System\.m_ShaderWatcher|m_System\.m_PendingReloads|m_System\.m_ReloadMutex|m_PickPending|m_PickCoord|m_PickedEntity" luth/source` returns 0 hits — every moved member is gone from the friend-access surface.
- Runtime smoke (user-tested): PBR + directional shadow + point lights + cascades + GTAO + bloom + skybox + outline + grid + ImGui render identically; LMB click selects entity, Ctrl-click toggles, Shift-click adds to selection, hierarchy drill-down works; editing `pbr.frag` triggers live reload + pipeline rebuild; Frame Debugger capture + replay + depth preview unchanged.

---

## Lessons

**A `no-op Update()` is sometimes the right `ISystem`.** `LightingSystem` has to run *inside* `RenderingSystem::Update` for the CPU flow (gather → cascade fit → UBO upload → global uniforms) to stay sequenced, not as an independent registry step. But making it `ISystem`-shaped with a no-op `Update` keeps it reachable via `SystemRegistry::GetSystem<T>()` — the same lookup pattern every editor panel already uses — with zero extra plumbing. Picking the right *shape* of extraction mattered more than picking the *placement* folder.

**Friend-class narrowing is an underrated cleanup lever.** A strict "ship only one class at a time" epic could have left `friend class RenderPipeline` untouched. But because `m_Cascades` / `m_ShadowParams` were *moving out* of `RenderingSystem` as part of A, Pipeline reaching across to read them became a layering violation *at that point* — not a future one. Fixing that one leg (via a `const CascadeData&` + `const DirectionalLightShadowParams&` parameter on `UpdateGlobalUniforms`) cost 20 lines and removed a real coupling. The remaining friend reads are all for state `RenderingSystem` legitimately owns — a proper `FrameContext` struct is a different epic.

**Architecture review closes on v2.6.0.** Seven epics (E1 `shader-asset-pipeline` → E7 `rendering-system-slim`) from the post-v2.0 review have shipped across v2.1.0-v2.6.0, all in a 36-hour window (2026-04-18 → 2026-04-19). RenderPipeline went from 3,104 LOC to 781; RenderingSystem went from 533 to 385; `glm::*` direct use is zero; `luth/core/` has three semantic sub-folders; `luth/animation/` is dissolved; single-stage shader assets are the only path. The feature backlog (`play-mode`, `jolt-physics`, `forward-plus`, …) resumes on a cleaner foundation than it paused on.
