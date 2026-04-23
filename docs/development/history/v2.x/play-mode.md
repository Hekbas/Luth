# v2.8.0 — play-mode

**Date:** 2026-04-23
**Commits:** 8 (on `feat/play-mode`)
**Issue:** [#66](https://github.com/Hekbas/Luth/issues/66)

---

## Overview

First feature epic after the post-v2.6 editor architecture review (EE1–EE6 shipped across v2.7.0–v2.7.5). Adds an **Editing → Playing → Paused → Editing** state machine to the editor, scene snapshot/restore for clean Stop reverts, and system-tick gating so game systems only advance during simulation. Gateway for v2.9.0 `jolt-physics` and any future gameplay-only system.

Core runtime: `Luth::PlayState` enum exposed via two new `IEditorHooks` methods (`GetPlayState`, `ConsumeStepRequest`) — engine stays unaware of editor internals; standalone runtime with no editor hook defaults to "always tick". `SceneSerializer` gains `SaveToString` / `LoadFromString(preserveAssets)` variants; the `preserveAssets=true` path keeps `Scene::m_HeldAssets` alive across the clear so `AssetManager` doesn't re-resolve every mesh/texture on Stop. Editor-side, new `PlayModeController` owns state + snapshot string + step flag; `CommandHistory` gets a block mode so Inspector edits during Play are no-ops (discarded on Stop anyway via snapshot rewind).

Editor UI: Play / Pause / Stop / Step transport buttons appended to `ScenePanel`'s toolbar after the gizmo tools; viewport border tint via `ImGui::GetForegroundDrawList()` — green (Playing), yellow (Paused); camera-source toggle (editor cam vs first Camera entity) visible only during Playing/Paused.

---

## Sub-Tasks

| # | Sub-task | Commit |
|---|---|---|
| A | `SceneSerializer` string I/O + `Scene::ClearPreservingAssets` | [`dcaad1e`](../../../../commit/dcaad1e) |
| B | `PlayState` enum + `GetPlayState`/`ConsumeStepRequest` hooks on `IEditorHooks` | [`2690a1f`](../../../../commit/2690a1f) |
| C | `PlayModeController` + `Editor::GetActiveScene`/`ResetDirtyState` helpers + `LuthienEditorHooks` overrides | [`a119799`](../../../../commit/a119799) |
| D | `App::Run` gates `AnimationSystem` by play state + preview toggle | [`8a5d037`](../../../../commit/8a5d037) |
| E | Transport bar + viewport tint in `ScenePanel` | [`bfd4d9c`](../../../../commit/bfd4d9c) |
| F | Block `CommandHistory::Execute/Undo/Redo` during play | [`11b577c`](../../../../commit/11b577c) |
| G | Scene-camera override during play + camera-source toggle | [`fe1fa88`](../../../../commit/fe1fa88) |
| H | Version bump + history + ROADMAP | this commit |

---

## Key Changes

- **New hook surface** — `Luth::PlayState { Editing, Playing, Paused }` in `luth/core/EditorHooks.h`; `IEditorHooks::GetPlayState()` and `ConsumeStepRequest()` both virtual with default impls (Editing, false). Default impls keep headless runtime nullptr-safe; `LuthienEditorHooks` overrides delegate to `PlayModeController::GetState()` / `ConsumeStepRequest()`.

- **`SceneSerializer` string I/O** — new `SaveToString(const Scene&) -> std::string` and `LoadFromString(Scene&, std::string_view, bool preserveAssets = false) -> bool`. File-based `Save`/`Load` delegate to the string variants. `preserveAssets=true` path uses new `Scene::ClearPreservingAssets()` (destroys entities via sparse-set loop, skips `ReleaseAllAssets`) so `AssetManager` isn't forced to re-resolve held meshes/textures on Stop. Two-pass reconstruction (create components → resolve parent/bone-attachment links) unchanged; per-entity uuids round-trip so existing held assets stay valid.

- **`PlayModeController`** — static class at `luthien/source/luthien/PlayModeController.{h,cpp}`. Owns `s_State`, `s_Snapshot` (JSON string), `s_StepRequested`, `s_SavedDirtyFlag`. `EnterPlay` captures the scene via `SaveToString`, snapshots `Editor::IsDirty()`, `CommandHistory::Clear() + SetBlocked(true)`, flips to Playing. `Stop` unblocks commands, `LoadFromString(preserveAssets=true)`, `Editor::ResetDirtyState(savedDirty)`, clears snapshot + step flag, flips to Editing. `Pause`/`Resume` just toggle state. `RequestStep` only valid when Paused; `ConsumeStepRequest` one-shot read.

- **Engine-side gating** — `App::Run` snapshots editor state once per frame (`EditorViewportState`, `PlayState`, `stepThisFrame = h->ConsumeStepRequest()`). Camera-params setup reused. `AnimationSystem::Update` gated by `runGameSystems = Playing || (Paused && step) || (Editing && previewAnimationInEditor)`. Headless runtime (`haveEditor = false`) always runs — standalone games don't carry PlayState machinery. `TransformSystem` / `RenderingSystem` / `PickingSystem` still tick unconditionally — viewport stays interactive.

- **`CommandHistory` block** — new `SetBlocked(bool)` / `IsBlocked()`. `Execute`, `Undo`, `Redo`, `BeginCompound`, `EndCompound` early-return when blocked. One warn log per block session (`s_WarnedBlocked` one-shot, reset on unblock). Play-mode UX: Inspector property edits and Ctrl+Z during Play produce nothing — scene snapshot restore handles any direct-registry pokes on Stop.

- **`Editor::ResetDirtyState(bool)`** — new helper sets `s_IsDirty` + `s_LastHierarchyVersion = scene->GetHierarchyVersion()` together. Prevents Editor's per-frame hierarchy-version delta check from re-flipping dirty after the Stop-time load (which bumps version via `Scene::ClearPreservingAssets` + entity creates). Existing `OpenScene`/`NewScene` could migrate to it for consistency (follow-up).

- **Transport bar (`ScenePanel` toolbar)** — inserted after gizmo tool buttons with vertical separator. Four buttons using `ICON_FA_PLAY` / `ICON_FA_PAUSE` / `ICON_FA_STOP` / `ICON_FA_FORWARD_STEP`. Enable-state matrix: Play in Editing, Pause in Playing, Stop in Playing+Paused, Step in Paused. Play button doubles as Resume when state is Paused (icon unchanged, tooltip shifts). Camera-source toggle (`ICON_FA_CAMERA`) only shown during Play/Pause, accent-tinted when scene camera is active.

- **Viewport tint** — `ImGui::GetForegroundDrawList()->AddRect(bounds[0], bounds[1], color, 0.0f, 0, 3.0f)` drawn after overlay pass. Green `IM_COL32(80,180,100,220)` for Playing, yellow `IM_COL32(220,180,80,220)` for Paused. ForegroundDrawList ensures the border overlays gizmos and overlays too — user sees the mode indicator regardless.

- **Scene-camera override** — `EditorViewportState` extended with `hasPlayCamera` + `playView` + `playProjection` + `playPosition`. Editor's `GetViewportState` walks `scene->Registry().view<Component::Camera, Component::WorldTransform>()`, picks the first match (entt iteration order — stable within a run). View = `Math::Inverse(xf.Matrix)`; proj = `Math::Perspective` or `Math::Ortho` + Vulkan Y-flip; position extracted from WorldTransform column 3. `App::Run` overrides `CameraParams` when `hasPlayCamera` is set, after the editor-cam setup. Fallback: if no Camera entity exists, a one-shot warn fires and editor camera continues. `EditorSettings::useEditorCameraInPlay` (default false) lets the user pin to editor cam across all play transitions.

- **Preview-animation toggle** — `EditorSettings::previewAnimationInEditor` (default true) persisted in `editor_settings.json`. Default preserves current UX (animations tick in Editing for preview). Setting routed through `EditorViewportState.previewAnimationInEditor` → gate in `App::Run`. Future "Preview Mode" toolbar affordance can flip this at runtime.

- **Scene dirty flag snapshot** — `PlayModeController::EnterPlay` saves `Editor::IsDirty()` into `s_SavedDirtyFlag`; `Stop` restores it via `Editor::ResetDirtyState(savedDirty)`. Prevents the play-session mutations + scene restore from flipping the editor dirty flag (which would prompt a stale Save dialog post-Stop).

---

## Design Decisions

- **PlayState editor-owned, not engine-owned.** Keeps engine unaware of editor concerns; standalone runtime has zero PlayState machinery and ticks normally. Engine reads through the existing `IEditorHooks::Get()` nullptr-safe pattern.

- **Pause = skip systems, not `Time::SetTimeScale(0)`.** Simpler — avoids subtle ordering risks with editor camera timing and matches issue wording literally ("game systems only tick in Playing/Paused+Step"). Future physics/scripting slot into the same gate by filtering on a new `ISystem::IsGameOnly()` or by explicit `Update<T>` wrapping in `App::Run`.

- **Snapshot as `std::string`**, not a cached JSON DOM. Reuses `SceneSerializer` 100%, memory-cheap even for large scenes, no new allocator pressure.

- **`LoadFromString(preserveAssets=true)` over a new "fast restore" path.** Avoided adding a second deserialization route; the single-flag branch inside `LoadFromString` swaps `Scene::Clear()` for `Scene::ClearPreservingAssets()` and the rest of the two-pass reconstruction is identical. UUIDs are preserved in the snapshot so the held-asset map remains valid references.

- **Block CommandHistory rather than lock hierarchy UI.** No disabled-UI ceremony, no per-panel guards. Snapshot rewind handles anything that bypasses commands (currently nothing — all mutations go through `CommandHistory::Execute`).

- **First Camera entity for scene-view override.** No "main camera" tag scheme yet; picks whatever entt iteration returns first (stable within a session). Unity-style tag / priority can bolt on later.

- **Single-frame step only.** No "step N frames" in this epic — kept the API minimal. Step granularity can extend to `RequestStep(int n)` if the need arises.

- **No `PlayModeEvent` EventBus broadcast yet.** Nothing subscribes today; will land when Physics/Scripting need it.

- **Animation `CurrentTime` pops to frame 0 on Stop.** Runtime animation state isn't serialized. Accepted for this epic; future follow-up can extend `SceneSerializer` to round-trip playback position if the pop is disruptive.

---

## Build Verification

- 8 atomic commits on `feat/play-mode`; every commit builds Debug x64 clean. Pre-existing warnings (LNK4006 in `shaderc_shared.lib`/`ws2_32.lib`/`dbghelp.lib`, C4996 `getenv`/`strncpy`, C4244 `Editor.cpp:400` chrono-to-uint) untouched.
- Runtime smoke (user-tested): Play enters, viewport tints green, animations run. Pause freezes animations, tint yellow. Step advances one frame, returns to paused. Stop reverts scene to pre-Play state (byte-identical after `SaveToString`). Transport button enable/disable matches state matrix. Camera-source toggle flips between editor and scene camera during Play. Preview-animation toggle off → animations frozen in Editing. Inspector property edits during Play produce a single warn log and no state change. Ctrl+Z during Play is no-op. Repeated Play→Stop cycles don't leak (`BoneMatrixBuffer` free-list stable).

---

## Next

v2.9.0 — `feat/jolt-physics`. Physics rigid bodies + colliders + Jolt-on-fiber `JPH::JobSystem`. Directly unblocks by `play-mode`: Physics will register as a game-only system and tick only in Playing / Paused+Step.
