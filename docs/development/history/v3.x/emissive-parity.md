# material-system.M.1 — emissive-parity

**Date:** 2026-06-09
**Commits:** on `feat/material-authoring` (renamed from `feat/emissive-parity`) — emissive: ST1 `507bffa`,
ST2 `429ebdd`, ST3 `6b6cb46`, ST4 `fc1bd02`, ST5 `4941683`; folded material-authoring fixes: `e61246f`
(factor controls), `4fb6ddb` (hot-reimport); wrap-up
**Issue:** [#152](https://github.com/Hekbas/Luth/issues/152)
**Umbrella:** [#151](https://github.com/Hekbas/Luth/issues/151)
**Series:** `material-system`, opener. Mode A series-coalesced — **v3.1.0** new-series MINOR bump +
milestone Release.

---

## Overview

Opener of the `material-system` arc. Fixes a **live raster≠RT bug**: `pbr.frag` declared
`emissiveIndex` / `FLAG_HAS_EMISSIVE` but its final write was `outColor = vec4(color, albedo.a)` —
**emission was never applied in raster**. The RT path *did* apply it (`geom_table.glsl`
`FetchHitSurface`), so emissive surfaces glowed in the path-traced reference / ReSTIR-GI / RT
reflections but were **black in raster** — violating the "PT reference is ground truth" contract
on `main`. The RT path also only sampled the emissive *texture* (no factor, no HDR strength).

M.1 adds a proper emissive model — **factor (linear color) × HDR strength**, optionally modulated by
the emissive texture — and applies the **byte-identical formula** in both `pbr.frag` and
`geom_table.glsl`, restoring raster == RT. HDR strength feeds bloom (automatic, via the existing
luminance-threshold extract on HDR sceneColor). Authoring lands in the editor (color + strength
controls) and on import (glTF `AI_MATKEY_COLOR_EMISSIVE` bridge), with a no-regression migration for
pre-existing textured emitters. A `ShadeMode::Emission` debug view isolates emission for a clean
raster-vs-PathTrace A/B.

**No new buffer, allocator, or sync primitive.** The change composes with existing primitives:
- the **direct-accessor** material pattern (mirrors `GetColor`/`SetColor` → `m_GPUData.color`) — the
  only material-data channel that actually reaches the GPU (the `u_*` dynamic-uniform path is dead:
  no Set-1 uniform block exists in `pbr`, so `u_EmissiveColor`/`u_Metalness`/`u_Roughness` never
  bridged);
- the **sizeof-strided** Material SSBO upload (`MaterialSystem::MATERIAL_SIZE = sizeof(GPUMaterialData)`)
  — growing the struct auto-grows the buffer, the stride, and the per-entry copy;
- the GPUTaggedPageAllocator-backed per-frame Material SSBO (unchanged).

Planned + Plan-agent-reviewed in plan mode (primitive inventory + arch-first per the engine's
plan-mode discipline). The strategic centerpiece — collapsing the duplicated BRDF into one shared
eval seam — is the **next** effort (M.2 `material-eval-seam`); M.1 deliberately keeps the two
emission sites in lockstep behind a numerical CONTRACT rather than unifying them yet.

---

## Sub-tasks

| # | What landed | Commit |
|---|---|---|
| ST1 | **Struct growth + plumbing (no visual change).** `GPUMaterialData` 64B → **80B**: appended `Vec4 emissive` (rgb = linear factor, a = HDR strength) after `flags` at byte 64 (std430 vec4-aligned). `static_assert(sizeof==80)` guards the stride (house convention — no `offsetof`). Direct accessors `Get/SetEmissiveColor` (Vec3) + `Get/SetEmissiveStrength` (f32). `Serialize`/`Deserialize` the `"emissive"` key; key-absence migration. All **three** GLSL mirrors grown in lockstep (stride-only): `pbr.frag`, `slim_gbuffer.frag`, `geom_table.glsl::GtMaterial` (`64 B`→`80 B` comment). | `507bffa` |
| ST2 | **Apply emission in both paths (parity).** `pbr.frag` computes `emission = emissive.rgb * emissive.a; if HAS_EMISSIVE: *= texture(emissiveIndex, UV0)` (before the shade-mode overrides) and writes `outColor = vec4(color + emission, albedo.a)` after the cascade-debug-mix. `geom_table.glsl` switches `s.emission` to the same factor×strength base then `*=` the texture (was a texture-only `=`). `// CONTRACT:` block in both files pins the math + the accepted `texture()`/`textureLod(.,0)` LOD asymmetry. | `429ebdd` |
| ST3 | **Authoring (editor + import).** `MaterialEditor` replaces the dead `u_EmissiveColor` picker with a direct LDR color swatch + a `DragFloat` HDR strength (both `MarkDirty`); drops the `u_EmissiveColor` skip in the Properties loop. `ModelImporter` bridges `AI_MATKEY_COLOR_EMISSIVE` into the direct `"emissive"` key (NOT the dead `u_*` channel); white-defaults the factor when only a resolved emissive texture is present. | `6b6cb46` |
| ST4 | **Inspector 3D-preview emission.** `thumbnail_mesh.{vert,frag}` PC gains `vec4 emissive` (vert carries it for cross-stage block parity; frag applies factor×strength — no texture, enough to preview the slider). `ThumbnailPreviewScene` PC struct 96B→112B + `RenderInspectorInternal` emissive param + fill from the material in `RenderMaterialInspector`. | `fc1bd02` |
| ST5 | **`ShadeMode::Emission` debug view.** Appended `Emission` to the `ShadeMode` enum (value 13 — append-only, no renumber of persisted `lastDebugMode`); in-shader `pbr.frag` override `if (v_ShadeMode == 13u)` reuses the ST2 `emission`; `ScenePanel` radio + `dbgActive` predicate. | `4941683` |

---

## Design decisions

### Emission formula — the raster==RT CONTRACT
`emission = emissive.rgb(linear) × emissive.a(strength)`, then `× emissiveTex(UV0).rgb` when
`FLAG_HAS_EMISSIVE` (bit 4) is set. Identical in `pbr.frag` and `geom_table.glsl`:
- No new flag bit — factor-only emission works without a texture (the flag gates only the texture
  multiply), and the existing bit-4 `HAS_EMISSIVE` (set when an emissive texture is present) suffices.
- Emissive has no UV-set bit in the flag schema (bits 16-23 cover diffuse/normal/metalrough/occlusion)
  → **always UV0**, matching the pre-existing RT behavior.
- The one accepted asymmetry: raster samples with `texture()` (mip/bias), RT with `textureLod(.,0)` —
  documented in both CONTRACT blocks as a pre-existing LOD difference (diffuse already differs the
  same way), not an algebraic divergence. The genuine math (factor×strength×texel) is byte-identical.
- The factor is pinned **linear** in the CONTRACT (the editor swatch is the LDR factor; strength
  carries the HDR range — the glTF `KHR_materials_emissive_strength` split).

### Direct accessors, not the dead uniform channel
`pbr.frag` / `pbr.vert` declare **no Set-1 uniform block**, so the `material.Set("u_*")` path silently
fails and never reaches the GPU. Emissive therefore uses direct `m_GPUData.emissive` accessors (the
working `color` pattern), serialized as a top-level `"emissive"` key. The editor's prior emissive
picker wrote the dead `u_EmissiveColor` uniform — rewired to the direct accessor.

### No-regression migration (two one-time seeds)
The new factor defaults to `{0,0,0,1}` (no emission). To avoid darkening existing/imported
emissive-*textured* assets that currently glow in RT:
- **Deserialize:** if the `.mat` predates the field (`!json.contains("emissive")`) **and** an emissive
  texture map is present → seed factor `(1,1,1)` strength `1`. Gated on key-absence only (never a
  value test) so it can never clobber a deliberate all-zero factor from a newer save.
- **Import:** the `ModelImporter` bridge writes the `"emissive"` key from the source factor; if only a
  *resolved* emissive texture node exists (factor absent/zero), it white-defaults — so the white-seed
  fires only for a genuinely present texture (avoids an unresolved-texture-glows-white trap).

### Struct growth is the blast radius, guarded by a size assert
`GPUMaterialData` is mirrored by **three hand-duplicated** GLSL structs (no shared include) + consumed
by three RT compute shaders via `#include geom_table.glsl`. A stride desync silently corrupts every
material index > 0. All three mirrors grow in ST1; `static_assert(sizeof==80)` (matching the house
`GPUObjectData==176` / `LightSSBOHeader==48` convention — `offsetof` is used nowhere) pins the C++
side. The upload is already `sizeof`-strided, so MaterialSystem needed no edit.

### `ShadeMode::Emission` appended, not inserted
The `ShadeMode` enum value is the raw `v_ShadeMode` integer the shader branches on. Appending
`Emission` (→ 13) avoids renumbering the persisted `EditorSettings.lastDebugMode` and the other
`ShadeMode` viz values; `pbr.frag` hardcodes `13u`, matching the existing 1u/3u/4u convention.

---

## Files touched

**Engine — material:** `renderer/material/Material.h` (struct field + accessors + size assert),
`renderer/material/Material.cpp` (serialize + deserialize migration).
**Engine — shaders:** `assets/shaders/pbr.frag` (emission compute + add + CONTRACT + `ShadeMode::Emission`
branch), `assets/shaders/common/geom_table.glsl` (factor×strength formula + CONTRACT + 80 B comment),
`assets/shaders/slim_gbuffer.frag` (stride mirror), `assets/shaders/thumbnail_mesh.{vert,frag}` (PC
emissive + preview apply).
**Engine — scene/import:** `scene/systems/RenderingSystem.h` (`ShadeMode::Emission`),
`resources/importers/ModelImporter.cpp` (emissive factor bridge).
**Editor:** `inspectors/MaterialEditor.cpp` (color + strength controls), `panels/ScenePanel.cpp`
(Emission radio + predicate), `widgets/ThumbnailPreviewScene.cpp` (PC + preview thread).
**Docs:** `core/Version.h` (3.1.0), `arch/rendering-pipeline.md` (Memory Budget 80 B), `ROADMAP.md`
(rt-renderer closed, material-system series added, gpu-particles re-homed, scripting retargeted).

---

## Verification

Build clean (Debug x64, luth → luthien → Runtime), no new warnings in touched files. All changed
shader stages + the three `geom_table.glsl` consumers (`path_trace.comp`, `restir_gi_initial.comp`,
`rt_reflections.comp`) pass `glslc --target-env=vulkan1.3` each sub-task. **Adversarial parity review
(ST2):** the two emission expressions confirmed algebraically identical — same `emissive.rgb*emissive.a`
base, same bit-4 flag gate, same `*=` texture-modulate at UV0, same additive final write; only the
documented LOD/binding asymmetry (shared with diffuse) differs. **Runtime smoke test before merge**
(visible UX): emissive color + strength glows in the Scene/Game viewport; `RenderMode::PathTrace`
matches the intensity (raster==RT); high strength blooms; non-emissive materials unchanged (no
white-glow regression); `ShadeMode::Emission` isolates emission; inspector mini-preview tracks the
slider; pre-existing emissive-textured assets still glow in RT and now in raster (migration).

---

## Folded material-authoring fixes

The branch was renamed `feat/material-authoring` and absorbed three long-standing inspector/asset bugs
into the same v3.1.0 release (one branch, no extra ceremony):

- **Factor controls reach the GPU (`e61246f`).** `metalness`/`roughness` joined `color`/`emissive` on
  the direct-accessor path (`Get/SetMetalness`, `Get/SetRoughness`) and now serialize + persist; the
  dead `u_*` uniform bridge in `UpdateGPUData` is gone. `ModelImporter` writes `color`/`metalness`/
  `roughness` as direct keys (was the dead `u_AlbedoColor`/`u_Metalness`/`u_Roughness`), and
  `Deserialize` migrates those legacy uniform values so existing imported materials recover their
  factors. Fixes the inspector metal/rough sliders snapping back to defaults + imported factors never
  reaching the GPU.
- **Emissive editable without a texture (`e61246f`).** The inspector's per-row `BeginDisabled` gate
  exempted only Diffuse/Metalness/Roughness; Emissive joined it, so a textureless material can set an
  emissive color + strength (factor-only emission already works in-shader).
- **No reimport bounce on editor self-writes (`4fb6ddb`).** Editing a material autosaves the `.mat`,
  which the FileWatcher saw as an external edit → `AssetManager::Evict` dropped the live instance the
  inspector held, so further edits stopped showing live ("hot modified + importing"). New
  `AssetDatabase::SuppressNextReimport(uuid)` records a self-write that `ProcessPendingChanges`
  consumes (still updating the artifact hash, so genuine external edits hot-reload as before).

---

## Hand-off / deferred

- **The strategic centerpiece is M.2** — collapse the hand-duplicated BRDF (the four-way
  "MUST stay algebraically identical" forms under `brdf.glsl`) into ONE shared evaluate-at-surface-point
  seam that both `pbr.frag` and `geom_table.glsl` call. M.1's emission CONTRACT is the proof-of-shape
  for that lockstep; the seam removes the duplication entirely.
- **Resolved this effort (`e61246f`):** the `metalness` / `roughness` / `base-color` **import** factors
  that hit the same dead `u_*` uniform channel — now on the direct-accessor path + legacy migration
  (see Folded material-authoring fixes above).
- **Inspector preview** shows factor×strength only (no emissive texture) — minimal scope; the main
  viewport shows the textured result. Promote to the full formula if the preview needs it.
- Git hooks not installed in this workspace — comment / commit policy honoured manually.
