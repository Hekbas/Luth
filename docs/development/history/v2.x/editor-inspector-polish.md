# v2.9.8 — editor-inspector-polish

**Date:** 2026-05-05
**Commits:** 20 (on `feat/editor-inspector-polish`)
**Issue:** [#117](https://github.com/Hekbas/Luth/issues/117)
**Series:** AAA editor rework, effort 9 of 9 (final before `editor-workspaces` series closeout)

---

## Overview

Inspector pass of the AAA editor rework. Eight planned sub-tasks plus
several test-driven refinements:

- Component header right-click ctx menu (Reset / Copy / Paste / Remove)
  with a typed editor-side clipboard (`entt::id_type`-keyed) and per-
  component JSON Copy/Paste in all 8 drawers.
- Asset slot composites the existing `ThumbnailCache` thumbnail over
  the button's left edge.
- New `widgets/InspectorHeader` (thumb-on-left + free-form right
  column, vertically centered against the thumb) used by Material /
  Model / Texture inspectors.
- New `widgets/Splitter` (horizontal drag strip with min/max clamp
  in-widget) and pinned-footer layout shared by all three resource
  inspectors.
- **Live interactive 3D preview** for Material (sphere) and Model
  (mesh) in the pinned footer — drag inside the area to orbit; reuses
  the v2.9.5 thumbnail bake pipeline through a new persistent 256²
  inspector RT (no readback).
- Modernized `Properties::DrawVecControlT` — colored axis Text labels
  (no full-button fill); right-click any axis → Reset / Copy / Paste
  whole vec.
- SceneViewer `(uuid, mtime)` parse cache eliminates the per-frame
  disk read + JSON parse.
- Editor-wide: 64-px FA-Solid + FA-Regular bakes for the ProjectPanel
  grid (downscale via `SetWindowFontScale` stays crisp at small thumb
  sizes); `widgets/ButtonGroup` icon buttons honor `FramePadding.x` so
  glyphs aren't pinched at default style.
- Long-standing fix: Remove Component re-checks `HasComponent<T>()`
  inside the open-header branch — the ctx-menu callback fires inside
  `BeginCollapsingHeader` and synchronously detaches T before
  `userDraw` runs.

Tag-only release. The series milestone Release stays reserved for
`editor-workspaces` (v2.9.9).

Effort scaled from M to L mid-flight when the user requested
interactive 3D previews + the unified pinned-footer layout (originally
the umbrella plan was Material/Model static thumbnail at top + Texture
footer only).

---

## Sub-Tasks

### Initial pass (A–G)

| # | Sub-task | Commit |
|---|---|---|
| A | NEW `EditorClipboard.{h,cpp}` — single-slot `entt::id_type`-keyed json store; main-thread only (ImGui contract) | [`3684406`](../../../../commit/3684406) |
| B | `ComponentDrawerOptions` + descriptor extended with `OnReset` / `OnCopy` / `OnPaste` lambdas; new `ComponentResetCommand<T>` mirroring `ComponentRemoveCommand` snapshot pattern | [`36de3b9`](../../../../commit/36de3b9) |
| C | Component header right-click ctx menu (Reset / Copy / Paste / Remove); per-component JSON `OnCopy` / `OnPaste` in all 8 drawers; `ComponentReplaceCommand<T>` for undoable Paste; Move Up/Down dropped (EnTT pools type-major) | [`560d165`](../../../../commit/560d165) |
| D | `widgets/AssetSlot::PropertyAsset` overlays `ThumbnailCache::Get` on the button left edge; icon fallback on cache miss (Material / Model / Texture only) | [`0c952c8`](../../../../commit/0c952c8) |
| E (initial) | MaterialEditor 192² preview region at the TOP of the inspector; explicit `ThumbnailCache::Invalidate` on per-release commit boundary | [`4c27e75`](../../../../commit/4c27e75) |
| F (initial) | ModelViewer 192² preview region at the TOP; cascade refresh handled by importer's `AssetChangedSignal` | [`9b30ef4`](../../../../commit/9b30ef4) |
| Fix | `ComponentDrawerRegistry`: re-check `HasComponent<T>()` inside `BeginCollapsingHeader`'s open branch — the ctx menu detaches T mid-frame on Remove | [`2e5e777`](../../../../commit/2e5e777) |
| G | SceneViewer `(uuid, mtime)` parse cache; replaces per-frame disk read + `nlohmann::json::parse` | [`f23b151`](../../../../commit/f23b151) |

### Test-driven refinements / scope expansion (L–O + I)

| # | Sub-task | Commit |
|---|---|---|
| L | NEW `widgets/Splitter.{h,cpp}` — horizontal drag strip; subtle 1-px center line idle, full-strip fill on hover/active; `ResizeNS` cursor | [`3d3bd35`](../../../../commit/3d3bd35) |
| M | NEW `widgets/InspectorHeader.{h,cpp}` (thumb + free-form right column); applied to Material (with Shader combo in right column) / Model (with summary line) / Texture (with dims + format + mip count); top static preview region from E + F removed (returns in pinned footer at O) | [`f3e890d`](../../../../commit/f3e890d) |
| H | TextureEditor pinned footer + splitter; `texturePreviewFooterHeight` field added to `EditorSettings` | [`cd38deb`](../../../../commit/cd38deb) |
| N | `ThumbnailPreviewScene::OrbitCamera` + `RenderMaterialInspector` / `RenderMeshInspector`; persistent 256² color + depth RT lazy-init on first call; `ImGui_ImplVulkan_AddTexture` once for the persistent ImGui descriptor; render-into-RT each call (no readback) | [`89249e6`](../../../../commit/89249e6) |
| O | MaterialEditor + ModelViewer: pinned footer + Splitter + orbit-cam input (drag inside footer area orbits the camera); shared `texturePreviewFooterHeight` setting; per-inspector `OrbitCamera` state (azimuth / elevation / distMul) | [`01909f5`](../../../../commit/01909f5) |
| I | `Properties::DrawVecControlT` modernized — colored axis Text labels (no full-button fill); drag-scrub via `DragFloat` unchanged; right-click any axis → Reset / Copy / Paste whole vec (`ImGui::SetClipboardText` format `(x, y, z)`) | [`c6c8657`](../../../../commit/c6c8657) |
| Fix | InspectorHeader `SetCursorPos({pad, pad})` replaces `Dummy + SameLine(0, 0)` (previous approach got only one of left/top padding); `MaterialEditor` `AlignTextToFramePadding` before "Shader" so it matches combo baseline; `topH = availH - footerH - splitterH - 2*ItemSpacing.y` in all three inspectors | [`a04dda4`](../../../../commit/a04dda4) |
| Fix | Footer **snapshot pattern** — `footerH_snap` frozen at frame start; Settings + Preview both size with the snapshot. Splitter writeback effective next frame so this frame's layout never overshoots `availH` (one-frame scrollbar flicker was visible when dragging UP); Splitter takes `minH` / `maxH` and clamps in-widget; absolute 400 px footer cap; InspectorHeader vertically centers the right column against the thumb (2-line estimate) | [`4812b95`](../../../../commit/4812b95) |

### Polish tweaks

| # | Sub-task | Commit |
|---|---|---|
| 1A | `widgets/ButtonGroup::IconBtnSize` returns `(GetFrameHeight + 2*max(0, padX-padY), GetFrameHeight)` — icon-only buttons gain horizontal breathing room while staying row-aligned with non-icon widgets at default style `{6,4}`. Applies to `IconToggleGroup` / `IconToggleButton` / `SplitToggleButton` icon-half | [`28239cc`](../../../../commit/28239cc) |
| 2B | Editor `FASolidLargeRef` + `FARegularLargeRef` accessors + 64-px `AddFontFromFileTTF`; ProjectPanel grid pushes the right large font + `SetWindowFontScale = (m_ThumbnailSize * 0.5f) / 64.0f` — bilinear minify keeps small / medium thumbs crisp; list mode unchanged | [`8f9a940`](../../../../commit/8f9a940) |
| Fix | ScenePanel toolbar fit: `iconBtnW` + `splitW` use the new icon-button width so `rightW` matches actual rendered width; `transportStart` / `rightStart` add `WindowPadding.x` (`SameLine(absX)` is window-relative, NOT content-relative — fixed off-by-padding under Custom / Matrix / Bubblegum styles); user icon swaps: gizmo Select `CROSSHAIRS → ARROW_POINTER`, shaded-wireframe placeholder `EARTH_AMERICAS → GLOBE` | [`ca8ca90`](../../../../commit/ca8ca90) |
| K | Wrap-up: Version.h, history file, ROADMAP, CLAUDE.md | this commit |

---

## Architectural decisions

### Footer snapshot pattern

Naive in-place mutation of `footerH` mid-frame breaks the layout
invariant. Settings is sized at frame start using `V_old`; Splitter
fires later (after Settings has rendered), mutates `footerH` to
`V_new` based on the drag delta; Preview is then sized using `V_new`.
When `V_new > V_old` (user drags up), total = `availH + (V_new - V_old)`
→ the parent's scrollbar appears for one frame.

The fix snapshots `footerH` to `footerH_snap` immediately after the
start-of-frame clamp. **Both** Settings AND Preview size with the
snapshot. The Splitter still mutates the persisted `footerH` (so
next frame picks up the new height) but this frame's layout always
sums to exactly `availH`. The drag is responsive (1-frame visual lag
is imperceptible at 60 fps) and there's never a one-frame overshoot.

This is also why `Splitter` takes `minH` / `maxH` and clamps in-widget
during drag — the clamp also prevents the persisted value from going
out of bounds, which would force the next frame's start-of-frame
clamp to silently round-trip.

### Live interactive 3D preview path

The umbrella plan called for a static thumbnail in the pinned footer,
but the user escalated to "real 3D model preview like in Unity, with
orbit camera at fixed distance, reusing the bake shader". Two options
considered:

1. Refactor `ThumbnailPreviewScene::BakeMaterial` / `BakeMesh` into
   a `RenderToTarget` API that takes caller-owned RTs + camera params.
2. Add a parallel `RenderInspector*` API alongside the existing bakes;
   keep both code paths separate.

Option 2 was chosen for v2.9.8 to ship within scope. The new
`RenderMaterialInspector` / `RenderMeshInspector` functions:
- Lazy-init a persistent 256² color + depth RT on first call. Caller-
  agnostic — same RT for Material and Model since only one resource
  inspector is visible at a time inside `InspectorPanel`.
- Register the color RT's view+sampler with `ImGui_ImplVulkan_AddTexture`
  exactly once; the descriptor outlives any single render call.
- Take an `OrbitCamera { azimuth, elevation, distMul }`. Distance is
  computed from the model's AABB (largest half-axis / tan(fov/2) ×
  distMul) so the model fits the frame regardless of size.
- Skip the readback path — render directly into the RT, return the
  ImTextureID. Each call is a fresh `ImmediateSubmit` (~1 ms).
- Layout transitions:
  `SHADER_READ_ONLY → COLOR_ATTACHMENT` (entry),
  `COLOR_ATTACHMENT → SHADER_READ_ONLY` (exit).
  Preserves the invariant that the RT is sample-ready when ImGui
  draws the descriptor.

The shared draw helper (`RecordRenderPass`) was deliberately NOT
extracted in this commit — the bake and inspector paths have similar
GPU work but different layout transition pairs (bake reads back via
`vkCmdCopyImageToBuffer`, inspector doesn't). DRY can come in a
follow-up if maintenance pressure shows up.

The orbit-cam input lives in the inspector body: `InvisibleButton`
sized to the preview area; on `IsItemActive` apply
`io.MouseDelta * sensitivity` to azimuth/elevation; clamp elevation
to `±85°`; `SetMouseCursor(ResizeAll)` while hovered or active.

### Move Up / Move Down dropped

EnTT stores components in type-major pools. Per-entity component
ordering metadata doesn't exist and adding it would touch
`Scene` / `Entity` in `Luth.lib` — out of scope for an editor-only
feature. The ctx menu ships with Reset / Remove / Copy / Paste only.
A future v3.x effort that adds entity-level component-ordering would
fold this in.

### Per-component JSON Copy/Paste lives in `Luthien.lib`

`SceneSerializer` (in `Luth.lib`) doesn't expose per-component (de)
serialization — it only reads/writes whole scenes. Two options for
the clipboard format:

1. Add per-component methods to `SceneSerializer` in `Luth.lib`.
2. Hand-write per-drawer JSON in `Luthien.lib`.

Option 2 chosen to preserve cornerstone 5 (editor decoupling). ~40
LOC of nlohmann::json calls duplicated across 8 drawers — the right
cost vs cracking the engine/editor seam for an editor-only feature.

### MaterialEditor live preview needs explicit `Invalidate`

`material.MarkDirty()` sets in-memory `m_GpuDirty` / `m_NeedsSave`
flags but doesn't publish `AssetChangedSignal`. `ThumbnailCache`'s
cascade subscriber listens for `AssetChangedSignal` only (file-watch
/ importer-write paths). In-editor live preview therefore calls
`ThumbnailCache::Invalidate` explicitly on each edit-cycle commit
(detected via `justReleased = m_SaveTimer == 0.0f` inside the auto-
save debounce branch).

ModelViewer is different — its Apply re-import path goes through
`AssetManager::Import` which DOES publish the signal, so cascade
handles it.

### `SameLine(absX)` is window-relative, NOT content-relative

`ImGui::SameLine(offset_from_start_x, default_spacing)` sets cursor
to `window.Pos.x + offset_from_start_x + spacing_w + GroupOffset.x +
ColumnsOffset.x`. With `default_spacing = -1`, ImGui clamps the
spacing arg to 0 (NOT to `Style.ItemSpacing.x`).

The relevant gotcha: `window.Pos.x` is the window's top-left in
screen coords — it doesn't include `WindowPadding.x`. So
`SameLine(rightStart)` puts the cursor at `WindowPadding.x` BEFORE
the content area's actual start.

Treating `absX` as content-relative requires explicitly adding
`WindowPadding.x`. The ScenePanel toolbar's `transportStart` and
`rightStart` were off-by-padding; under the Rider style
(`WindowPadding.x = 2`) the gap was barely noticeable, but Custom /
Matrix (`{8, 8}`) and Bubblegum (`{12, 12}`) showed it clearly.

### `IconBtnSize()` honors `FramePadding.x` asymmetry

`GetFrameHeight()` is `FontSize + 2 * FramePadding.y` — uses Y
padding only. Strict-square icon buttons sized at
`(GetFrameHeight, GetFrameHeight)` ignore `FramePadding.x`, which
makes the button visually pinched horizontally when `padX > padY`
(Custom / Matrix `{6, 4}` defaults).

`IconBtnSize()` returns
`(GetFrameHeight + 2 * max(0, padX - padY), GetFrameHeight)` —
matches what a natural-sized button would be width-wise (text-width
+ 2 * padX), keeps the height equal to other row neighbors. Applied
uniformly to `IconToggleGroup` / `IconToggleButton` /
`SplitToggleButton` icon-half.

### Multi-bake font for ProjectPanel grid icons

The default font atlas bakes FA-Solid at ~16 px. Drawing it at
4× scale to fit a 96 px thumbnail cell is bitmap upscale → blurry.

Approach taken: bake a second FA-Solid font at 64 px during
`LoadFonts` (and a 64-px FA-Regular for the empty-folder case).
ProjectPanel grid pushes the right large font and applies
`SetWindowFontScale((m_ThumbnailSize * 0.5f) / 64.0f)` — that's
downscale (bilinear minify, clean) for thumbs ≤ 128, upscale only
beyond that (which still looks acceptable for the worst case).

The whole-asset-icon-as-baked-thumbnail approach (`Option D` in the
v2.9.8 discussion) was deferred — it composes with `ThumbnailCache`
+ cascade infra and is the cleaner long-term answer, but represents
its own follow-up effort.

### Vec drag widget visual change preserves DragFloat semantics

The umbrella plan's "drag the label to scrub" wording implied a
custom drag widget. Reality check: `EditState.committed` semantics
from v2.9.6 depend on `IsItemDeactivatedAfterEdit` — replacing
`DragFloat` with custom `InvisibleButton` + `MouseDelta` math would
reinvent that. Solution: keep `DragFloat` (unchanged drag-scrub),
just swap the visual (filled axis button → colored Text label) and
move click-to-reset into the new right-click ctx menu.

---

## Files modified

| File | Change |
|---|---|
| `luthien/source/luthien/EditorClipboard.{h,cpp}` | NEW — single-slot typed clipboard |
| `luthien/source/luthien/widgets/Splitter.{h,cpp}` | NEW — horizontal drag strip with min/max clamp |
| `luthien/source/luthien/widgets/InspectorHeader.{h,cpp}` | NEW — thumb-on-left + free-form right column |
| `luthien/source/luthien/widgets/Widgets.h` | Add Splitter + InspectorHeader includes |
| `luthien/source/luthien/widgets/ButtonGroup.cpp` | `IconBtnSize` honors `FramePadding.x - FramePadding.y` |
| `luthien/source/luthien/widgets/ThumbnailPreviewScene.{h,cpp}` | NEW `OrbitCamera` + `RenderMaterialInspector` / `RenderMeshInspector` + persistent 256² inspector RT + ImGui descriptor |
| `luthien/source/luthien/widgets/AssetSlot.cpp` | Composite `ThumbnailCache::Get` thumbnail over button left edge; icon fallback |
| `luthien/source/luthien/widgets/Properties.cpp` | `DrawVecControlT` colored Text labels + right-click ctx menu (Reset / Copy / Paste whole vec) |
| `luthien/source/luthien/inspectors/ComponentDrawerRegistry.h` | `OnReset` / `OnCopy` / `OnPaste` fields on options + descriptor; `entt::id_type ComponentTypeId` for clipboard keying; ctx-menu lambda extends to Reset / Copy / Paste / Remove; re-checks `HasComponent<T>()` after ctx menu |
| `luthien/source/luthien/commands/ComponentCommands.h` | NEW `ComponentResetCommand<T>` + `ComponentReplaceCommand<T>` |
| `luthien/source/luthien/inspectors/component_drawers/{Transform,Camera,MeshRenderer,Animation,BoneAttachment,AnimationController,DirectionalLight,PointLight}Drawer.cpp` | Per-component `OnCopy` / `OnPaste` lambdas (nlohmann::json) |
| `luthien/source/luthien/inspectors/MaterialEditor.{h,cpp}` | `OrbitCamera` member; thumb-on-left header (Shader combo in right column); pinned footer + Splitter + interactive preview; `ThumbnailCache::Invalidate` on commit; static top preview removed (relocated to footer) |
| `luthien/source/luthien/inspectors/ModelViewer.{h,cpp}` | `OrbitCamera` member; thumb-on-left header (summary line); pinned footer + Splitter + interactive preview; static top preview removed |
| `luthien/source/luthien/inspectors/TextureEditor.cpp` | Thumb-on-left header (dims + format + mips); pinned footer + Splitter; settings region scrolls above |
| `luthien/source/luthien/inspectors/SceneViewer.{h,cpp}` | `(uuid, mtime)` parse cache replaces per-frame disk read + JSON parse |
| `luthien/source/luthien/EditorSettings.{h,cpp}` | `texturePreviewFooterHeight` field + JSON load/save |
| `luthien/source/luthien/Editor.h` | `m_FASolidLarge` + `m_FARegularLarge` + accessors |
| `luthien/source/luthien/EditorStyle.cpp` | `LoadFonts` adds 64-px FA-Solid + FA-Regular bakes |
| `luthien/source/luthien/panels/ProjectPanel.cpp` | Grid pushes `GetFASolidLarge` / `GetFARegularLarge` + `SetWindowFontScale` for crisp icons at large thumbnail sizes |
| `luthien/source/luthien/panels/ScenePanel.cpp` | `iconBtnW` formula matches `IconBtnSize`; `transportStart` / `rightStart` add `WindowPadding.x`; gizmo Select icon swap (CROSSHAIRS → ARROW_POINTER); shaded-wireframe icon swap (EARTH_AMERICAS → GLOBE) |
| `luth/source/luth/core/Version.h` | `VERSION_PATCH` 7 → 8 |

---

## Out of scope (deliberate)

- **FontViewer `stb_truetype` glyph render** — needs `stb_truetype.h`
  vendored to `luth/extern/source/stb/`. Deferred to a follow-up
  effort. The placeholder TextWrapped strings stay for now.
- **Move Up / Move Down on component header** — EnTT pools are
  type-major. Documented in the spec's Context section.
- **`RenderToTarget` refactor of `BakeMaterial` / `BakeMesh`** — the
  parallel `RenderInspector*` path ships within v2.9.8 scope. DRY of
  the shared GPU work is a candidate follow-up.
- **`Option D` from the icon-scaling discussion** (whole-asset-icon-as-
  baked-thumbnail uniformly across all asset types) — composes with
  `ThumbnailCache` cascade but represents its own polish epic.
- **Multi-select inspect** — high value, ~150-200 LOC + drawer
  signature changes. Slot for v2.10.x post-`jolt-physics` if it
  still feels needed.
- **Per-entity component search** — useful for entities with many
  components, rare today. Revisit when `RigidBody` / `Collider`
  components land in `jolt-physics`.
- **Sub-task Q (orbit-camera fly mode / WASD inside the inspector
  preview)** — the static-distance orbit is the v2.9.8 floor.

---

## Verification

Manual smoke test in editor (no automated harness for ImGui interactions):

| # | Case | Result |
|---|---|---|
| 1 | Right-click any component header → Reset / Copy / Paste / Remove | OK |
| 2 | Reset Values restores defaults; Ctrl+Z restores prior values | OK |
| 3 | Copy Component on entity A → Paste on entity B transfers values; Ctrl+Z restores B's prior; Paste disabled on type mismatch | OK |
| 4 | Remove Component on Camera no longer crashes (was failing on `HasComponent<T>` assert) | OK |
| 5 | MeshRenderer Material slot + Material albedo/normal/etc. slots show mini thumbs after first bake; other slot types fall back to icon | OK |
| 6 | Material inspector: thumb-on-left header + Shader combo in header right column; pinned footer 3D preview drag-orbits; releasing a property re-bakes the asset slot thumbnail (next frame) | OK |
| 7 | Model inspector: thumb-on-left header + summary line; pinned footer 3D preview drag-orbits; Apply re-import refreshes the preview | OK |
| 8 | Texture inspector: thumb-on-left header + dims line; pinned footer; settings scroll above splitter | OK |
| 9 | Drag splitter UP — no scrollbar flicker at any drag speed (footer snapshot pattern) | OK |
| 10 | Drag splitter — capped at 400 px max; can't shrink below 80 px min | OK |
| 11 | SceneViewer entity count / file size: cached on `(uuid, mtime)`; external save (touch file) refreshes next frame | OK |
| 12 | Vec3 Position/Rotation/Scale: colored X/Y/Z text labels (no buttons); drag-scrubs; right-click any axis → Reset / Copy / Paste whole vec; clipboard format `(x, y, z)` | OK |
| 13 | Single Ctrl+Z per slider release (v2.9.6 commit semantics intact) | OK |
| 14 | ProjectPanel grid icons scale crisply with thumbnail size; empty folders use FA-Regular at the same scale | OK |
| 15 | Scene toolbar buttons aren't pinched horizontally (default style); right block ends exactly at the panel right edge under Custom / Matrix / Rider styles | OK |

---

## Tagging

```bash
git checkout main
git merge --no-ff feat/editor-inspector-polish -m "feat(release): merge feat/editor-inspector-polish (#117)"
git tag -a v2.9.8 -m "v2.9.8 — editor-inspector-polish"
git push origin main --follow-tags
```

Mode B tag-only — no GitHub Release. The `editor-workspaces` (v2.9.9)
effort is the candidate for the AAA editor rework series milestone
Release.
