# packed-texture-routing (v3.2.4)

**Date:** 2026-06-14 · **Issue:** [#160](https://github.com/Hekbas/Luth/issues/160) (Part of [#157](https://github.com/Hekbas/Luth/issues/157)) · **Series:** `slang-material` Phase 3 · **Branch:** `feat/packed-texture-routing`

## Summary

Phase 3 fixes "the original pain" the slang-material arc was opened to solve: the importer mangled custom-packed and non-standard texture layouts. ORM, separate metal+rough, spec-gloss, and DirectX (green-inverted) normal maps now remap into the bounded material channels **at import time**, so `material.slang`'s single fixed-swizzle decode (metalRough `.g`/`.b`, occlusion `.r`, normal `*2-1`) always reads canonical data. The remap is entirely CPU-side and upstream of the GPU, so it required **zero** changes to `GPUMaterialData`, `material.slang`, or the boot-time `MaterialLayoutGuard`, and raster/RT parity is preserved by construction (both `RasterFetch` and `RayFetch` read the same artifacts and SSBO through the same decode).

Three mechanisms, in increasing weight: material-level index routing (ORM aliasing, no pixel work), single-file pixel transforms (green-flip, gloss-invert), and a derived-texture bake (separate metal+rough combine, spec-gloss conversion). The bake writes real PNG assets next to the model and registers them through the normal pipeline, composing with the existing embedded-texture-extraction path rather than introducing a synthetic asset type.

## What shipped

### 1. TextureRole + import-side channel transforms

`TextureRole { Color, NormalGL, NormalDX, LinearData, GlossToRoughness }` on `TextureSettings` (import-time only, read from the `.meta` `role` key). `TextureImporter` applies a per-texel transform after `Image::Load` and before serialize, so the bindless artifact already carries canonical bytes:
- `NormalDX`: flip the green channel (`g = 255 - g`) so the decode's `*2-1` yields the +Y normal the BRDF expects.
- `GlossToRoughness`: invert the green channel (roughness in the glTF metalRough convention) so the decode's `.g` reads roughness.
- `Color` / `NormalGL` / `LinearData`: no-op (zero cost on the common path).

The transform runs on a worker fiber (no Vulkan); mips are generated later on GPU upload from the canonical bytes, and the per-texel affine ops commute with the box-downsample so every mip agrees.

### 2. TextureBaker (synthesized canonical textures)

New `TextureBaker` writes a baked RGBA8 PNG into the model's `_Textures/` dir and registers it as an ordinary Texture asset (`MetaFile::Create` + `AssetDatabase::RegisterAsset`), records its source UUIDs as `.meta` dependencies, and drops any stale artifact so the next load reimports. Baked maps get a pinned role (`LinearData` for data, `Color` for converted baseColor) so `TextureImporter` leaves their already-canonical bytes alone. `Image::SavePng` (a thin `stbi_write_png` wrapper) was added for the disk write.

- `BakeMetalRough`: pack a separate roughness map (into G) and metalness map (into B) into one metalRough map, resizing mismatched inputs to a common size first.
- `BakeSpecGlossToMetalRough`: the Khronos glTF-appendix metallic-solve. Per texel, solve metallic from the diffuse/specular luminances against the dielectric F0 (0.04), lerp baseColor between the dielectric and metal reconstructions by `metallic^2`, and set roughness = `1 - glossiness`. Emits **two** maps (baseColor + metalRough) with all factors folded in.

### 3. ModelImporter routing rework

`ProcessMaterial`'s texture handling was rebuilt around three lambdas: `ResolveSlot` (Assimp slot to UUID + source path + UV, recording unresolved), `AddNode` (push a `.mat` texture node, one per canonical slot), and `StampRole` (write an inferred role into the texture's `.meta`, never clobbering a user-set role, invalidating the artifact on a fresh stamp). Routing rules:
- **Dedup fix:** METALNESS and DIFFUSE_ROUGHNESS resolving to the **same** file stay one Metalness node (the glTF combined-map happy path). Resolving to **different** files is the separate-maps case (previously the dedup silently dropped roughness).
- **ORM aliasing:** a metalRough texture whose stem is `_orm` / `_arm` / `_rma` also aliases into the occlusion slot when no dedicated AO map filled it (its R channel feeds the occlusion read).
- **Separate metal+rough:** baked into one packed map via `TextureBaker::BakeMetalRough`; floors to roughness-only + scalar metalness with an `ImportReport.Degraded` entry if the bake fails.
- **Spec-gloss:** detected as a specular map with no metal-rough; converted via `BakeSpecGlossToMetalRough`, swapping the raw diffuse node for the converted baseColor and neutralizing the scalar color (the decode multiplies color * baseColor).

Role auto-detection prioritizes the Assimp semantic type, then refines by filename stem (`_dx` to NormalDX, `_gloss` to GlossToRoughness, `_orm`/`_arm`/`_rma` to ORM aliasing). DirectX normals are not reliably filename-detectable, so normals default to GL and rely on a `_dx` suffix or the editor override. A `ModelImportSettings.AutoDetectTextureRoles` knob disables the heuristic per-model. The now-dead `AssimpToLuthMapType` helper was removed.

### 4. Editor override + degraded surface

`TextureEditor` gains a Channel Role combo in Import Settings: it reads/writes the `.meta` `role` key and reuses the existing Apply path (`meta.Save` then delete artifact then `AssetManager::Import` then `Evict`), so a mis-detected DirectX normal is a two-click fix. The `Editor` import-report check logs a one-line reduced-fidelity summary alongside the existing `TextureRemapDialog`.

## Design decisions / deviations

- **Derived-texture bake included (user-chosen).** The plan offered a "correctness floor now, defer the bake" option; the user chose the full bake so separate metal+rough and spec-gloss are faithful, not floored. This is why the effort landed at L rather than the S-M the ROADMAP sketched.
- **No new asset type for baked textures.** A baked map is a real PNG registered through the normal pipeline, mirroring how embedded textures are already extracted to disk and registered. This composes with load / bindless / GC / thumbnails / inspect for free and avoids inventing a synthetic-UUID concept.
- **No automatic reverse-dependency cascade.** The asset pipeline has no general dependents index today (only the `.frag` to `.vert` special case in `ProcessPendingChanges`). A baked texture therefore refreshes on model reimport or an editor Apply, not automatically when an input source edits. The recorded `.meta` dependencies make a future cascade straightforward; for Phase 3 it is unnecessary because `MaterialSystem::Update` re-resolves each material's bindless indices every frame, so a reimported texture propagates next frame with no material reimport.
- **Role stored import-time-only.** The transform bakes into the artifact, so `TextureRole` is not echoed in `TextureHeader` (no runtime or GPU need); the editor reads it back from the `.meta` sidecar. A doc comment notes the future-sRGB seam: such a pass would select sRGB-view vs UNORM off the role (Color sRGB, data roles UNORM).
- **sRGB left orthogonal.** All bindless textures sample as UNORM (the v3.1.6 sRGB attempt was reverted), so the green-flip / gloss-invert / spec-gloss math are linear-space byte ops, correct for and independent of the current no-sRGB sampling. The spec-gloss conversion treats source bytes as linear; it is approximate by nature (spec-gloss to metal-rough is lossy) and uses the industry-standard solve rather than chasing bit-exactness.

## Files

- **New:** `resources/importers/TextureBaker.{h,cpp}`
- **Engine:** `renderer/resources/Texture.h` (TextureRole + TextureSettings.Role), `resources/importers/TextureImporter.cpp` (ApplyRoleTransform), `resources/Image.{h,cpp}` (SavePng), `resources/importers/ImportReport.h` (DegradedTexture), `resources/importers/ModelImporter.{h,cpp}` (routing rework + AutoDetectTextureRoles), `core/Version.h`
- **Editor:** `inspectors/TextureEditor.{h,cpp}` (Channel Role combo), `Editor.cpp` (degraded summary)
- **Docs:** `ROADMAP.md` (Phase 3 row), `epics/slang-material.md` (tracker)

## Sub-tasks (commit order)

| # | Commit | Notes |
|---|--------|-------|
| 1 | `feat(renderer): add TextureRole + import transforms` | enum + TextureSettings.Role + ApplyRoleTransform |
| 2 | `feat(assets): add TextureBaker for synthesized textures` | Image::SavePng + BakeMetalRough + register path |
| 3 | `feat(assets): route packed textures + auto-detect roles` | resolve/route rework, ORM alias, dedup fix, role detect, Degraded |
| 4 | `feat(assets): convert spec-gloss to metal-rough at import` | BakeSpecGlossToMetalRough + ModelImporter detect/convert |
| 5 | `feat(editor): TextureRole override + degraded notice` | TextureEditor combo + Editor summary |

## Verification

Build clean after each sub-task (Debug x64). Runtime smoke (import an ORM glTF, a DirectX-normal model, a separate-metal+rough asset, a spec-gloss model, a standalone `_orm` png) is the user-run gate before merge: confirm canonical channels in-editor and raster vs RT/path-trace parity. The Phase-0 `SlangParityGuard` SPIR-V gate is unaffected (no shader change).
