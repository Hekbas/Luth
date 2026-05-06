# v2.9.5 — editor-thumbnails

**Date:** 2026-05-03
**Commits:** 17 (on `feat/editor-thumbnails`)
**Issue:** [#114](https://github.com/Hekbas/Luth/issues/114)
**Series:** AAA editor rework, effort 6 of 8

---

## Overview

ProjectPanel grid switches from FontAwesome-icon-only to **rendered thumbnails
for textures, meshes, and materials**. Falls back to the FA icon when a bake
isn't ready. Disk-persisted at `<project>/.luth/thumbnails/<uuid>.png`; async
generation off the main thread; per-frame dispatch + drain budgets keep cold-
start smooth.

The architecture spec for Pillar 8 (`lets-plan-this-in-humble-lovelace.md`)
targeted **v1 = texture-only + icon fallback** with mesh / material deferred
to a future `thumbnail-rtt` epic. v2.9.5 instead pulled all three generators
into one effort — foundation (cache, signal sub, completion plumbing, disk
persist) is shared, and the GPU bake path was simpler than the spec implied
once the design landed on `ImmediateSubmit` + a custom Lambert shader instead
of a pass appended to the main render graph.

A drift-fix mid-effort centralised every stb_image touch point in a new
`Luth::Image` module — single site for the global flip flag, no more race
between callers. `VKTexture(const fs::path&)` (dead code) was deleted as a
free side dividend.

Tag-only release. The series milestone Release is reserved for `editor-workspaces`
(v2.9.7).

---

## Sub-Tasks

| # | Sub-task | Commit |
|---|---|---|
| A | `ThumbnailCache` foundation + AssetChangedSignal subscription + ProjectPanel hook | [`f5f4dd3`](../../../../commit/f5f4dd3) |
| B | Texture thumbnail generator (CPU stbi_load → stbir_resize → stbi_write_png) | [`0dd85d0`](../../../../commit/0dd85d0) |
| C | Disk persistence + startup scan + orphan GC | [`e50036f`](../../../../commit/e50036f) |
| — | Fix: throttle wrap on ScanDiskCache + ImGui pool destroyed before deletion-queue flush at close | [`6bce5b8`](../../../../commit/6bce5b8) |
| — | Fix: drop the leak-prone in-flight counter; defensive try/catch in worker / Drain | [`5cdb0fa`](../../../../commit/5cdb0fa) |
| — | Fix: dispatch FIFO not LIFO so within-folder load order matches reading order | [`ab5c228`](../../../../commit/ab5c228) |
| — | Fix: stb-flip race + bake aspect-preserving + mipmaps; tighter ProjectPanel display | [`698bfd0`](../../../../commit/698bfd0) |
| — | Centralise image decode in `Luth::Image`; delete dead `VKTexture(path)` | [`1174866`](../../../../commit/1174866) |
| — | Fix: defer thumbnail dispatch via FIFO queue + lower per-Drain cap | [`c3a9628`](../../../../commit/c3a9628) |
| D | Mesh thumbnail bake (preview-scene infra + generator wiring) | [`ad1c216`](../../../../commit/ad1c216) |
| — | Skinned mesh thumbnail variant (second pipeline, stride 84 B) | [`b3e402d`](../../../../commit/b3e402d) |
| — | Fix: bake every sub-mesh of a multi-mesh Model | [`37a3d70`](../../../../commit/37a3d70) |
| F | Material thumbnails + cascade invalidation | [`11e72cd`](../../../../commit/11e72cd) |
| — | Fix: material thumbnails sample the albedo texture (direct binding, not bindless) + tighter camera fit | [`49b9c4e`](../../../../commit/49b9c4e) |
| G | `EditorSettings` thumbnail controls (`thumbnailsEnabled`, `thumbnailMaxDiskEntries`) | [`f55648c`](../../../../commit/f55648c) |
| — | ProjectPanel layout polish (Selectable cells, 3-line truncation, list-view thumbnails, padding) | [`a5c71dc`](../../../../commit/a5c71dc) |
| H | Wrap-up: docs + version bump + history | this commit |

E was merged into D — the bake infrastructure isn't testable without the
generator hookup, and splitting them produces one dead-code commit followed
by a small wiring change. Same total diff, cleaner bisect milestone.

---

## Architectural decisions

### Big-bang scope (all three generators in one effort)

Pillar 8 v1 was texture-only with mesh / material deferred. The user picked
**Option 2 (big-bang)** at planning time. The shared foundation (cache map,
SpinLocks, signal sub, completion plumbing, disk persist, per-frame dispatch
budget) is ~70% of the total work; building it once for textures and a
second time for mesh / material would have doubled review surface for limited
incremental value.

### Texture path: CPU-resize PNG, not bindless alias

The architecture spec sketched "leverages existing bindless / `UI::GetTextureID`
pipeline." Plan-agent audit identified that aliasing the source texture's
bindless descriptor for a 64-px thumbnail forces a full-resolution load of
every project texture — a 4K HDR is 64 MB of VRAM for one preview cell.
Industry convention (Unity `Library/PreviewCache/`, Unreal `FAssetThumbnailPool`,
Godot `.thumbcache`) is to persist a small downsampled image to disk and load
it as a tiny VkTexture. Worker fiber decodes via `stbi_load`, resizes via
`stbir_resize_uint8`, encodes via `stbi_write_png_to_func`, persists via
`IOThread::WriteFile`. Main-thread `Drain` creates a small `VKTexture` from
the RGBA8 bytes and registers an ImGui descriptor. Source asset never enters
the AssetManager cache for thumbnail-only consumption.

### Mesh / material bake: synchronous on main via `ImmediateSubmit`

The spec called for "a one-off pass appended to the main per-frame
RenderPipeline graph." The PBR pipeline samples Set 0–5 with per-frame
GlobalUniforms / Material SSBO / Lights UBO / Object SSBO data. Reusing it
for thumbnails would require either piggy-backing on those SSBOs (slot-share
with the main view) or duplicating the descriptor infrastructure for the
bake. Both are non-trivial and tangled with V3 (cmd-buffer affinity) +
V6 (allocator-reset) hazards.

The simpler design: a dedicated thumbnail pipeline with its own minimal shader
(custom `thumbnail_mesh.vert/.frag` — Lambert + ambient × albedo color,
push-constants only), no descriptor sets except the per-bake sampler. Bake runs
synchronously on main via `VulkanContext::ImmediateSubmit`. ImGui pool +
descriptor pool stay isolated from the main render pipeline.

Cost: ~10–30 ms blocking the main thread per bake. Capped at **1 GPU bake per
frame** by the cache's dispatch budget — at 60 Hz that's a 1-frame budget hit
for each mesh / material thumbnail. For 50 thumbnails: ~1 second to fully
populate, smooth pacing throughout.

### Direct sampler binding, not bindless

The first material-bake attempt sampled via the engine's bindless array using
`material->GetGPUData().diffuseIndex`. The bindless slot is registered **after**
the texture's upload fence retires (per `texture-async-uploads` v2.8.14's
deferred-bindless-registration pump). On a fresh project where the texture
wasn't already cached, the slot was still 0 (white default) at the moment we
sampled — material thumbnails came out as just the tinted color.

Fix: bypass bindless. Pipeline owns its own `VkDescriptorSetLayout` with
`set=0 binding=0` = combined image sampler. Per-bake `vkUpdateDescriptorSets`
points it at the material's albedo `VKTexture::GetImageView()` /
`GetSampler()` directly — those are valid the moment the VKTexture
constructor returns (`CreateViewAndSampler` runs synchronously in the ctor).
A `vkDeviceWaitIdle` after `LoadImmediate` ensures the pixel-data upload
fence has actually retired before we sample.

Mesh bakes bind a 1×1 white default texture (created at Init via
`Texture::Create` + `vkDeviceWaitIdle`) so the same shader works for both
paths.

### Two pipeline variants (static + skinned)

`Vertex` is 52 B, `SkinnedVertex` is 84 B. `Position@0` and `Normal@12` and
`TexCoord0@24` are at identical offsets in both — only the per-vertex stride
differs. Two pipelines that share the same shader code and differ only in
`VkVertexInputBindingDescription.stride` cover both cases. Skinned bakes
render bind pose; bone data is ignored. Matches Unity / Unreal's "skinned
character renders bind pose in project panel" convention.

### Multi-mesh model handling

`model->GetMesh(0)` was insufficient — FBX / GLTF assets routinely split a
single model across body / hair / glass / wheels sub-meshes. Bake collects
**every** sub-mesh into a draw list before `ImmediateSubmit`, takes the
**combined bind-pose AABB** for the camera fit, and inside one
`vkCmdBeginRendering` pass binds pipeline + push constants once and
switches `VkBuffer`s per sub-mesh. Single submission covers the whole model.

### Camera fit: max-axis half-extent + 1.1 padding

Initial fit used `Math::Length(aabb.Extents())` (bounding-sphere radius =
sqrt(3) × half-axis for uniform shapes) + 1.4 padding. For a unit sphere at
FOV 45°, that fills ~43% of the frame — visibly tiny. Switched to **max
half-axis** with 1.1 padding: ~95% of FOV for spheres / cubes. Trade-off:
elongated shapes seen at extreme orbit angles can lightly clip corners; the
frame's far plane still uses `Length(extents) × 4` so depth coverage is
preserved.

### `Luth::Image` centralisation (mid-effort drift fix)

`stbi_set_flip_vertically_on_load` is a process-global int. Three callers
set it inconsistently (`TextureImporter` = 0, `VKTexture(path)` = 1,
`IBLPrecompute` = 1). Concurrent loads raced — whichever caller set it last
won. The architecture also has fibers that migrate between OS threads, so
stb's `_thread` variant isn't an option.

`luth/source/luth/resources/Image.{h,cpp}` is now the only site that touches
the flag. `Image::Init()` runs once at App boot, sets it to 0, never
changes it. All callers go through `Image::Load` / `Image::LoadFromMemory`
/ `Image::LoadHDR` / `Image::FlipVertical*` / `Image::EncodePngToMemory` /
`Image::Resize`. `TextureImporter`, `IBLPrecompute`, `ThumbnailGenerator` all
migrated. Old `ImageUtils.cpp` (the impl-defines TU) folded into `Image.cpp`.

`VKTexture(const fs::path&)` and its public factory `Texture::Create(const fs::path&)`
were dead code — confirmed zero callers across engine + editor (asset
pipeline goes through the data-taking ctor exclusively). Deleted as part of
the migration.

### Stutter mitigation: deferred dispatch + per-type budget

First post-fix complaint: cold-start stutter on entering a new texture folder.
The throttle counter approach (cap concurrent bakes at 8) was leak-prone —
any path that incremented without decrementing wrapped the `u32` to
`UINT32_MAX` and permanently saturated the throttle.

Replaced with a **deferred-dispatch FIFO queue**: `Get` and `ScanDiskCache`
push `DispatchRequest{asset, type, fromDisk}` into `s_PendingDispatches`;
`Drain` pumps **5 texture dispatches + 1 GPU bake per frame** from the front
of the queue. FIFO order matches reading order so within-folder thumbnails
populate top-down.

The two budgets reflect the cost asymmetry — texture bakes run on workers
(~1 ms CPU), GPU bakes block main (~10–30 ms). 1 GPU bake / frame keeps
frame pacing smooth even when many materials are queued.

### Cascade invalidation for material → texture deps

When user edits a sampled texture, dependent material thumbnails should
re-bake. `BakeMaterialSync` captures `material->GetTextures()`'s UUIDs into
the cache entry's `deps` field; the `AssetChangedSignal::Modified` handler
walks `s_Entries` under `s_MapLock` to collect material thumbnails whose
`deps` contain the changed UUID and invalidates them outside the lock.

Edge-frequency event — V1 micro-critical budget doesn't apply to the walk.

### `s_TextureCache` (TexturePreview.cpp) deliberately not folded

The Pillar 8 spec called for folding `s_TextureCache` into `ThumbnailCache`.
On reflection these are distinct abstractions: `ThumbnailCache` is
**UUID-keyed asset thumbnails** (project-asset surface); `s_TextureCache`
is **raw-`Texture*`-keyed runtime previews** (e.g., `SceneColor`,
runtime-generated textures with no UUID). They serve different needs and
coexist in v2.9.5. A future polish epic can revisit if a unified API ever
has a clean shape.

### Engine-wide bug uncovered along the way (Bug B)

`ImGui_ImplVulkan_RemoveTexture` lambdas pushed via `VulkanContext::PushDeletion`
were firing **after** `ImGui_ImplVulkan_Shutdown` destroyed the descriptor
pool — null-deref crash on close. The latent issue affected the existing
`s_TextureCache` (`TexturePreview.cpp`) too; the new `ThumbnailCache` only
made it more visible.

`Editor::Shutdown` now drains `VulkanContext::FlushAllDeletionQueues()`
**before** `ImGui_ImplVulkan_Shutdown`. Pending lambdas fire while ImGui
Vulkan is still alive; ImGui then tears down with all descriptors freed.
Fixes the close crash for both caches.

### V1-V6 hazard mapping

| Hazard | Status | Note |
|---|---|---|
| V1 lock contention | **mitigated** | All cache mutations use `SpinLock` micro-critical sections (insert/find under `s_MapLock`, queue swap under `s_QueueLock`/`s_DispatchLock`); cascade-invalidation walk runs at edge frequency, exempt from the <100-cycle budget |
| V2 main-thread starvation | **mitigated** | Per-frame dispatch budget (5 texture + 1 GPU bake) caps main-thread cost; texture bakes run on workers |
| V3 cross-thread cmd-buffer | **mitigated** | All bake work uses `ImmediateSubmit` (synchronous, single cmd-buffer, single fiber); never yields under recording |
| V4 lost wakeup | N/A | No sleep/wait protocol |
| V5 sub-job context thrash | N/A | No nested dispatch |
| V6 GPU↔allocator deadlock | N/A | Bake target is a dedicated persistent VkImage owned by `ThumbnailPreviewScene`, not a per-frame allocation |

---

## Files & locations

### New — engine

- `luth/source/luth/resources/Image.{h,cpp}` — centralised stb_image / write
  / resize wrapper. `Init` sets stb's global flip flag to 0 once, never again.
  `ImageUtils.cpp` (the previous impl-defines TU) deleted.
- `luth/extern/source/stb/stb_image_resize.h` — vendored single-header.
- `luth/extern/source/stb/stb_image_write.h` — vendored single-header.
- `luth/assets/shaders/thumbnail_mesh.{vert,frag}` + `.meta` — push-constant-
  driven Lambert + ambient × albedo with per-bake sampler binding at
  `set=0 binding=0`.

### New — editor (luthien)

- `luthien/source/luthien/widgets/ThumbnailCache.{h,cpp}` — UUID-keyed asset
  thumbnail cache with SpinLock-guarded map + completion queue + dispatch
  queue + AssetChangedSignal subscription with cascade invalidation +
  `ScanDiskCache` for `<project>/.luth/thumbnails/`.
- `luthien/source/luthien/widgets/ThumbnailGenerator.{h,cpp}` — async
  dispatch entry. Texture path runs on workers (CPU decode + resize +
  encode + IO write); mesh + material run synchronously on main
  (`LoadImmediate` + `ThumbnailPreviewScene::BakeMesh / BakeMaterial`).
- `luthien/source/luthien/widgets/ThumbnailPreviewScene.{h,cpp}` — owns the
  bake pipeline (static + skinned variants), persistent 128² color RT +
  D32 depth + host-mapped staging buffer for readback, persistent sampler
  descriptor pool / layout / set, 1×1 white default texture, lazy-loaded
  Sphere primitive for material bakes. `BakeMesh` / `BakeMaterial` issue
  one `ImmediateSubmit` per call.

### Modified — engine

- `luth/source/luth/core/App.cpp` — `Image::Init()` wired in App ctor.
- `luth/source/luth/core/Version.h` — bumped to `v2.9.5`.
- `luth/source/luth/renderer/backend/vulkan/VulkanTexture.{h,cpp}` —
  deleted dead `VKTexture(const fs::path&)` ctor + the `<stb/stb_image.h>`
  include.
- `luth/source/luth/renderer/lighting/IBLPrecompute.cpp` — migrated to
  `Image::LoadHDR` + `Image::FlipVerticalF32`.
- `luth/source/luth/renderer/resources/Texture.{h,cpp}` — deleted dead
  `Create(const fs::path&)` factory.
- `luth/source/luth/resources/importers/TextureImporter.cpp` — migrated to
  `Image::Load`; old `stbi_set_flip_vertically_on_load(0)` removed.

### Modified — editor (luthien)

- `luthien/source/luthien/Editor.cpp` — `ThumbnailCache::Init / Shutdown /
  Drain / Clear` wiring; `ThumbnailPreviewScene::Init / Shutdown` wiring;
  `OnProjectChanged` calls `ScanDiskCache` after `Clear`. **Engine-wide
  fix**: `VulkanContext::FlushAllDeletionQueues()` before
  `ImGui_ImplVulkan_Shutdown` so PushDeletion lambdas don't fire against
  a destroyed pool at close.
- `luthien/source/luthien/EditorSettings.{h,cpp}` — two new fields
  (`thumbnailsEnabled`, `thumbnailMaxDiskEntries` — default 10000,
  `0 = unbounded`).
- `luthien/source/luthien/panels/ProjectPanel.{h,cpp}` — `DrawItem`
  rewritten around `ImGui::Selectable` covering the full cell; selection
  visual via `ImGuiCol_Header`; click + double-click + drag-drop +
  context-menu binding all consolidated; `DrawTruncatedTextWrapped` helper
  caps grid labels at `k_MaxNameLines = 3` with ellipsis; list view
  switched to actual thumbnail (line-height aspect-fit, FA glyph fallback);
  `k_ListModeThreshold` 16 → 32; `WindowPadding` / `ItemSpacing.y` tuning.

### Modified — docs

- `docs/development/ROADMAP.md` — v2.9.5 row in completed table; dropped
  from planned.
- `CLAUDE.md` — Current Progress block updated (untracked).
- `docs/development/history/v2.x/editor-thumbnails.md` — this file.

---

## Build Verification

17 commits on `feat/editor-thumbnails`; every commit builds Debug x64 clean
(pre-existing C4996 / C4244 warning baseline only — one new warning from
upstream `stb_image_write.h`'s `sprintf` use, isolated to extern). Smoke
test confirms:

- **Texture thumbnails.** Open project with PNG / JPG textures — within
  ~1 sec per visible cell, the actual resized image appears. PNGs land in
  `<project>/.luth/thumbnails/<uuid>.png`. Source textures NOT loaded into
  AssetManager (`ResourcePanel` confirms).
- **Mesh thumbnails.** FBX / GLTF / DAE models show a Lambert-shaded
  bind-pose render. Multi-mesh assets show all sub-meshes framed together.
  Skinned models show bind pose (no animation).
- **Material thumbnails.** Sphere with the material's albedo texture
  sampled and tinted by `GetColor()`. Materials without an albedo map
  show flat color × white default.
- **Cascade invalidation.** Edit a sampled texture (re-save in external
  editor) → dependent material thumbnails re-bake within 1–2 frames.
- **Disk persistence.** Close + reopen project — thumbnails reload from
  disk cache without re-baking. Delete a project asset, restart — orphan
  PNG GC'd.
- **No first-folder stutter.** 50+ texture folder loads top-to-bottom over
  ~10 seconds with smooth frame pacing throughout.
- **Layout polish.** Grid cells: aspect-correct image centered; up to
  3-line ellipsised name; whole cell clickable; `ImGuiCol_Header`
  selection highlight. List mode at slider 16-32: small thumbnail on
  left, name on right.
- **Editor close.** Clean shutdown, no `ImGui_ImplVulkan_RemoveTexture`
  null-deref (Bug B fix).
- **Y-flip eradicated.** `Luth::Image::Init` sets stb's flip flag once;
  no caller mutates afterward; all images load top-left origin
  consistently across asset types.

Closes #114.
