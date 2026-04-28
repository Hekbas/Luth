# v2.8.8 — animation-quick-pass

**Date:** 2026-04-28
**Commits:** 8 on `refactor/animation-quick-pass` (7 sub-tasks + 1 hotfix)
**Issue:** [#93](https://github.com/Hekbas/Luth/issues/93)

---

## Overview

Promotes `AnimationClip` from an inline value on `Model` to a first-class UUID-addressable `Asset`. `ModelImporter` writes one `.anim` source per clip into `<stem>_Animations/` (mirroring `<stem>_Materials/` and `<stem>_Textures/`); `AnimationClipImporter` cooks each into a `.luth` artifact under `Library/Artifacts/`. The Model artifact bumps V2 → V3 (UUID list trailer instead of inline clip data); V2 is rejected on read so projects re-import once on first open after the upgrade.

Runtime references shift from integer indices to UUIDs — `Animation::AnimationIndex` becomes `Animation::ClipUUID`, `BlendLayer::ClipIndex` / `AnimationTransition::FromClip`/`ToClip` / `AnimationController::CurrentClipIndex` likewise switch to `UUID`. `AnimationSystem` samples by direct `AssetManager::GetAsset<AnimationClip>(uuid)` lookup — same hash-table-read cost as the existing model lookup. `SceneSerializer` migrates legacy `animationIndex` / `clipIndex` payloads on first load via a blocking `LoadImmediate` against the entity's model. Editor inspectors swap `ImGui::Combo` for the existing `UI::PropertyAsset` drag-drop slot (`PERSON_RUNNING` icon).

Foundation for v2.11's `animation-controller-v2` (state machine + blend trees) — clips are rig-agnostic at the asset boundary, but tracks still encode bone *indices* not bone names, so two characters can share one `.anim` only if their skeletons agree on bone ordering. Bone-name retargeting stays in scope for v2.11. Tag-only release; no user-visible feature lands here on its own.

> **Deviations from #93.** `BACKLOG.md` listed two goals — preview-toggle UX (clearer indicator + transport-bar surfacing for `previewAnimationInEditor`) and rig/clip decoupling. Only the second shipped; preview-toggle UX was deferred.

---

## Sub-tasks

| # | Sub-task | Commit |
|---|---|---|
| A | `AnimationClip` inherits `Asset`; `.anim` extension + `Animation` enum entry | [`e674bff`](../../../../commit/e674bff) |
| B | `AnimationClipImporter` (identity validate + write); AssetManager registration | [`d863ca3`](../../../../commit/d863ca3) |
| C | `ModelImporter` writes sibling `.anim` per clip; Model V2→V3 with V2 reject; transitional UUID lookups in consumers | [`ff27444`](../../../../commit/ff27444) |
| D | `Animation::AnimationIndex` (i32) → `ClipUUID` (UUID); SceneSerializer + EntityCommands migration | [`2f69373`](../../../../commit/2f69373) |
| E | Controller `BlendLayer`/`AnimationTransition`/`AnimationController` → UUID fields; `Play(UUID)` | [`8d60beb`](../../../../commit/8d60beb) |
| F | Drawer combos → `UI::PropertyAsset` drag-drop; `PERSON_RUNNING` icon | [`d7c8344`](../../../../commit/d7c8344) |
| G | `AssetDatabase::ProcessPendingChanges` Modified branch evicts cached asset (was stale post-recook) | [`9bfbd0e`](../../../../commit/9bfbd0e) |
| – | Smoke fix: widen filename sanitize set for FBX clip names containing `\|` `?` `*` `<` `>` `"` | [`c4274e9`](../../../../commit/c4274e9) |
| – | Wrap-up (this file, version bump to 2.8.8, ROADMAP/BACKLOG updates, merge + tag) | (this commit) |

---

## Architectural changes

- **`AssetType::Animation`** — appended to the enum after `Scene`. Inserting mid-enum would shift the `AssetHeader.Type` ints stored in every existing artifact (Texture, Material, Shader, Font, Scene), forcing parallel version bumps. Append-only is the AAA pattern (UE writes `FName` strings, Unity pins explicit class IDs); Luth's enum-cast-to-int is fragile, but pinning explicit values is a separate chore deferred until needed.

- **`AnimationClip : public Asset`** — was a plain struct; now carries `Handle` / `Flags` / `LastAccessedTime` from the base. Field-by-field serialization is unchanged; the inherited members add a vtable + 24 bytes that don't appear in the `.luth` payload.

- **`Model::m_AnimationClipUUIDs : vector<UUID>`** — replaces `vector<AnimationClip> m_AnimationClips`. `GetAnimationClip(u32)` removed; `GetAnimationClips()` becomes `GetAnimationClipUUIDs()`. The `Skeleton` stays inline on `Model` — bone hierarchies are still per-rig, only the clip data decouples.

- **`AssetSerializer` Model V2 → V3.** V2 wrote inline `WriteAnimationClips` after the skeleton; V3 writes `UUID[AnimationCount]`. The header version check rejects anything other than V3 (mirrors the Shader V1→V2 reject pattern at `AssetSerializer.cpp:412`). The V1 backward-compat path (pre-v2.4 model artifacts) was deleted along with V2 — both are forced through the importer on first open.

- **`AnimationClipImporter`** — minimal identity-and-write. Source `.anim` files are byte-identical to artifacts (ModelImporter writes the cooked binary directly into the assets folder). Import deserializes + re-serializes through `AssetSerializer::Deserialize/SerializeAnimation`, which both validates the header and produces an artifact under `Library/Artifacts/<uuid>.luth`. A standalone clip-only FBX import path (drop `walk_only.fbx` with no mesh) is deferred — current `ModelImporter` always assumes geometry.

- **`ModelImporter` sibling emission.** After `ExtractAnimationClips`, the importer writes `<source_stem>_Animations/<clipName>.anim` for each clip when `ExtractClipsAsSeparateAssets` is set on the `.meta` (default true). Each side file gets a `.meta` via `MetaFile::Create` and registers with `AssetDatabase::RegisterAsset` — identical pattern to `<stem>_Materials/` (lines ~750–758 of ModelImporter.cpp). UUIDs are looked up first via `AssetDatabase::GetUUID(clipPath)` so re-imports keep stable IDs across FBX edits. Duplicate clip names get an `_<idx>` suffix.

- **Filename sanitize set.** FBX clip names from layered rigs commonly carry `|` (e.g. `Mammals|idle_A1` — confirmed in user smoke test). The sanitize set includes the full Windows-reserved set: `:` `/` `\` `|` `?` `*` `<` `>` `"`. Existing material-name sanitize (`ProcessMaterial` in same file) covers only the path separators — that path is undisturbed since Assimp FBX material names rarely contain the others.

- **`AssetManager::Update` Model dep load.** When a Model finalizes, the post-load hook now triggers `LoadAsync` for every entry in `m_AnimationClipUUIDs` (mirrors the existing Material → Texture pattern). Without this, AnimationSystem's first-frame `GetAsset<AnimationClip>` would return null and entities would render bind-pose for one frame.

- **Two-pass migration.** `SceneSerializer` and `EntityCommands` read both `clipUUID` (new) and `animationIndex` / `clipIndex` (legacy). Legacy resolution takes the entity's `Animation::ModelUUID`, calls `AssetManager::LoadImmediate` (blocking), and looks up `Model::GetAnimationClipUUIDs()[idx]`. The lookup is cached per controller so an N-layer Controller triggers at most one `LoadImmediate`. Legacy fields will be dropped in v2.9.0; for now both shapes are accepted on read, only `clipUUID` is written.

- **`ctrl.CurrentClipIndex` mirror dropped.** The single-clip path's `anim.AnimationIndex = ctrl.CurrentClipIndex;` sync at the end of the controller-time-advance block (sub-task E) becomes a direct `anim.ClipUUID = ctrl.CurrentClipUUID;` — same intent (Animation drawer reflects the controller's selection), no index round-trip through the model's clip list.

- **`UI::PropertyAsset` extended for `AssetType::Animation`.** Single switch arm in `AssetSlot.cpp` adds `ICON_FA_PERSON_RUNNING`. Project Panel already emits `"ASSET_UUID"` payloads for `.anim` files (it's type-agnostic), so no panel work needed. The drawer changes net −35 LOC across three files — combo + clipNames-storage scaffolding deleted.

- **`AssetDatabase::ProcessPendingChanges` Modified branch evicts.** The existing path removed the artifact and queued the asset for re-import but left `s_Assets[uuid]` populated, so `GetAsset` continued returning the stale in-memory copy. The fix calls `AssetManager::Evict(uuid)` after the artifact removal — the next `GetAsset` returns null, the next `LoadAsync` re-cooks and re-uploads. Type-agnostic: improves Texture / Material / Shader hot-reload along with `.anim`. Anything still holding a `shared_ptr` keeps the old data alive until release (use-after-free not possible).

---

## Build verification

All commits build clean Debug x64 on Windows MSVC. No new warnings; pre-existing only (`Editor.cpp` chrono cast, `vulkan-1.lib` `LNK4006` import-descriptor warnings, `ProjectLauncher.cpp` `getenv`, `InspectorPanel.cpp` `strncpy`).

Smoke tests (user-driven, all pass):

- Open project, observe `Model V2 → V3` reject + auto-reimport of every FBX. `<stem>_Animations/` folders populate with `.anim` + `.meta` files. `Library/Artifacts/<uuid>.luth` exists per clip.
- Pre-existing scene loads; `SceneSerializer: migrated Animation index X -> ...` for each animated entity. After save + reopen, YAML/JSON contains `clipUUID` instead of `animationIndex`.
- Single-clip Animation: AssetSlot displays clip name + `PERSON_RUNNING` icon; speed / timeline / transport buttons unchanged; loop-all cycles through model's UUID list.
- AnimationController: crossfade transitions, bone-mask layers, root motion all unchanged. Per-layer + current-clip slots accept drag-drop.
- Drag-drop: `.anim` from Project Panel → AssetSlot swaps clip live. Wrong-type drop rejected with warn log. Right-click Clear → bind pose fallback.
- Hot-reload: touch a `.anim` mtime → `Hot-modified ... queued for reimport` log → entity updates within ~1 sec without restart. Delete a referenced `.anim` → `Hot-removed` log, bind-pose fallback, no crash.

---

## Out of scope (deliberately)

- **Bone-name retargeting.** Tracks still carry `BoneIndex` (resolved during import against the source skeleton), so cross-rig sharing requires identical bone ordering. The headline workflow win — one `walk.anim` driving two different characters — only fires when both rigs were exported with the same hierarchy. Real retargeting (bone-name lookup at sample time, optional remap table) is in scope for `animation-controller-v2` (v2.11.0).

- **State machine + blend trees.** `AnimationController` keeps its current shape (layered overrides + simple crossfade). The state machine and blend-tree authoring layer is `animation-controller-v2`'s job.

- **Standalone clip-only FBX import.** Dropping `walk_only.fbx` (animation tracks but no mesh) into the assets folder still routes through `ModelImporter`, which produces a Model artifact even when the only thing extracted is a skeleton + clips. A dedicated path that recognizes mesh-less FBXs and emits only `.anim` siblings is a future polish item.

- **`AssetType` enum hardening.** Append-at-end works today but won't survive an inattentive reordering. Pinning explicit values + a `static_assert` (or moving to FNV1a hashes per AAA convention) is its own chore.

- **Preview-toggle UX.** BACKLOG's first goal — clearer indicator when `previewAnimationInEditor` is on, surface in transport bar — is independent of the asset refactor. Deferred to a future polish item.

- **`ModelViewer` clip drag source.** The Animations table in ModelViewer can be a drag *source* (each row carries a clip UUID) but currently isn't. Project Panel drag-drop covers the headline workflow; ModelViewer drag is an editor convenience for later.
