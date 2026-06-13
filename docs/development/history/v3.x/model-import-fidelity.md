# v3.1.6: model-import-fidelity

**Date:** 2026-06-12
**Branch:** `feat/model-import-fidelity`, commits `ccdd683`, `9128bd2`, `b5221cc`, `6d6ba2d`, `51985be`, `bd82fe7`, `d0552f1`, + wrap-up
**Version:** v3.1.6 PATCH bump, tag-only (Mode A cadence).

---

## Overview

The Luth runtime was far ahead of its Assimp importer. The `Material` asset, the shaders, and the
pipeline already supported Opaque/Cutout/Transparent, alpha cutoff, cull mode, a second UV set,
occlusion, etc. (and the `MaterialEditor` exposed all of it), but `ModelImporter` under-populated
almost everything: render mode hardcoded to Opaque, no alpha cutoff / two-sided, occlusion texture
never assigned, UV1 never written, and the DCC scene graph discarded (node transforms baked into
vertices, everything flattened, cameras and lights dropped entirely).

This effort closes that gap. Scope was set with the user up front: **faithful node graph** (entity per
node, not baked extraction); spot/area lights to `PointLight` + warning; **texture color-space
correctness** (attempted, then reverted; see below).

## What shipped

- **Material import correctness + UV1** (`ccdd683`): glTF `alphaMode`/`alphaCutoff` + opacity to
  `render_mode` + `alpha_cutoff`; `AI_MATKEY_TWOSIDED` to `CullMode::None`; occlusion texture assigned
  (`AMBIENT_OCCLUSION` + `LIGHTMAP` fallback); metal-rough deduped onto the one sampled slot
  (`MapType::Metalness`); `Vertex.TexCoord1` populated; per-map `uvIndex` read from `AI_MATKEY_UVWSRC`.
  No artifact bump (`.mat` is JSON).
- **Model V4 + cameras/lights/node graph** (`b5221cc`): new `ModelNode` / `ModelCamera` / `ModelLight`
  on the Model; `AssetSerializer` V4 appends a TRS node tree + camera + light blocks (rejects V3,
  forces reimport). Static models reconstruct the `aiNode` hierarchy with **un-baked** mesh-local
  vertices; axis correction + scale factor fold onto the root node. Cameras (`mHorizontalFOV`
  half-angle to vertical degrees), lights (spot to point + warn, intensity split out of color
  magnitude, range from attenuation). Thumbnail preview renders sub-meshes at their node-world
  transforms (folded into viewProj per draw, no shader change).
- **Engine-side instantiation** (`6d6ba2d`): `Scene::InstantiateModel` (engine, zero `luthien/`
  includes, runtime-reusable) walks the V4 tree to one entity per node with decomposed TRS, attaches
  `MeshRenderer` / `Camera` / `DirectionalLight` / `PointLight`, parents per the tree. The skinned
  path (bone-entity hierarchy) is preserved unchanged and gated on `Model::HasNodeTree()`.
  `ModelInstantiateCommand` thins to a wrapper keeping the `SerializeEntitySubtree` undo snapshot.
  Import-options toggles (cameras/lights) in `ModelViewer`.
- **glTF buffer copy on import** (`d0552f1`): `AssetDatabase::IngestFile` copies a `.gltf`'s referenced
  `.bin` buffer(s) into the project beside the model. Assimp loads the buffer directly (not via the
  texture resolver), so it must sit next to the `.gltf`; previously only the model file + adjacent
  images were copied, so a glTF with an external `.bin` failed to import.

## Key design decisions

- **Bifurcate static vs skinned at the file level.** Node tree present iff static iff all verts
  un-baked. Skinned files keep the entire prior path (baked verts in skeleton space, bone entities);
  the skinning math is normative (`arch/animation-system.md`) and the lethal failure was applying a
  node transform to already-baked verts. Skinned files emit no node tree.
- **Store TRS, not Mat4, in V4.** Decompose happens at import anyway; storing TRS avoids per-load
  `glm::decompose` and round-trip drift. (The *which* decompose mattered; see the rotation bug.)
- **Engine-side `Scene::InstantiateModel`.** Node-to-entity construction is a pure engine concern
  (Model + Scene + Components + AssetManager). Moving it out of the editor command satisfies the
  editor-decoupling cornerstone and frees a future runtime/game load path. The one deliberate
  architectural move this effort makes.
- **Un-bake fallout is mostly free.** Render / RT / GPU-cull already apply `world x local`, so
  un-baked static meshes need no change there (`TransparencySubsystem` already multiplies the sort
  center by the model matrix; the editor AABB gizmo already transforms local corners). Only the
  thumbnail (which renders the Model asset directly, not via entities) needed node-world transforms.

## The rotation bug, and a latent finding

First smoke test: **some objects rotated ~180 degrees.** The first hypothesis (negative-scale / mirror
nodes mishandled by `glm::decompose`) was wrong, and committing it before user verification was a
process miss (it didn't fix it; reverted).

Root-caused empirically with a headless `decompose to euler to reconstruct` round-trip test over ~17k
rotation/scale combos: **`Luth::DecomposeTransform` (LuthMath.h) applies `glm::conjugate`, which
inverts the rotation** in this glm version (error scales with angle: a pure 25-degree Z-rotation
already fails; 13,161/17,576 combos fail). The fix is to decompose **without** the conjugate
(`Math::Decompose`); failures drop to 0 for all non-gimbal cases. (The residual 204 are exact-90-degree
Euler gimbal lock, inherent to the Transform component storing Euler degrees, not this path.)

The conjugate is a **latent bug masked everywhere it's currently used**: animation drives skinning from
the bone-matrix buffer (not the bone entity `Transform`), and physics reads quaternions directly;
neither round-trips a rotation through `Transform.Rotation` to render. Node import is the first path
that does. **Fixed importer-locally** (`51985be`, user-verified); the shared `DecomposeTransform` was
left untouched (removing its conjugate globally needs a dedicated effort that re-verifies animation +
physics). **Flagged as a follow-up.**

## sRGB texture color-space, attempted then reverted

Added a `RGBA8_SRGB` format and per-map color-space tagging (`9128bd2`) so albedo/emissive decode from
sRGB while normal/MR/AO stay linear. The *render* path is genuinely correct for it (linear lighting,
`pow(1/2.2)` encode, UNORM swapchain), but it surfaced two blockers: the **editor's ImGui display path
isn't sRGB-aware** (it hardware-decodes the sRGB view and writes linear to the UNORM swapchain, so
every preview/thumbnail renders dark), and the `.meta` `srgb=true` default is wrong for data maps that
aren't auto-assigned by the importer. Proper gamma correctness is a coordinated effort (sRGB-aware
editor display via UNORM alias views, correct per-type defaults, material re-tune), not a side-effect
of import work. **Reverted (`bd82fe7`)** to the engine's prior consistent all-UNORM behavior; old sRGB
artifacts self-heal to UNORM on load (the removed enum value falls through to the UNORM default).

## Verification

- Debug x64 MSBuild clean after each sub-task (Luth.lib + Luthien.lib + Luthien.exe), 0 errors.
- Rotation round-trip diagnosed + fix confirmed via a headless doctest (`13161` to `0` failures); test
  removed after root-causing.
- User runtime smoke: faithful node-graph import with correct orientation; cameras/lights as entities
  with gizmos; material render modes / occlusion / UV1; skinned models unchanged; texture appearance
  restored after the sRGB revert.

## Files

**Importer/assets:** `resources/importers/ModelImporter.{cpp,h}`, `resources/AssetSerializer.{cpp,h}`,
`resources/AssetManager.cpp`, `renderer/resources/Model.{cpp,h}`.
**Engine scene:** `scene/Scene.{cpp,h}`.
**Editor:** `commands/AssetCommands.cpp`, `inspectors/ModelViewer.cpp`, `widgets/ThumbnailPreviewScene.cpp`.
**Reverted (sRGB):** `renderer/resources/Texture.h`, `backend/vulkan/VulkanTexture.cpp`,
`importers/TextureImporter.cpp`, `inspectors/TextureEditor.{cpp,h}`.

## Deferred / follow-ups

- **`DecomposeTransform` conjugate**: global fix + re-verify animation/physics.
- **Gamma correctness**: sRGB-aware editor display, per-map-type meta defaults, material re-tune.
- **Euler gimbal lock** at exact +/-90 degrees (Transform stores Euler): quaternion storage would remove it.
- Mesh-node shear (dropped by TRS decompose), embedded raw textures (still skipped): pre-existing,
  rare.
