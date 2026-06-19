# vertex-deformation (D1–D4)

**Date:** 2026-06-19
**Commits:** 23 on `feat/vertex-deformation` (`c2168220` … `f303493a`)
**Issue:** [#161](https://github.com/Hekbas/Luth/issues/161)
**Series:** standalone arc, Mode A — series-open MINOR bump **v3.2.8 → v3.3.0**, one history file, one `--no-ff` merge, one tag.

---

## Overview

A GPU **deformation seam**: one graphics-queue compute pass at frame start writes each deformable mesh's
per-asset double-buffered "deformed buffer" (interleaved `Vertex`, 52 B/vert × 2 curr/prev regions), which
BOTH the raster vertex shaders AND the RT BLAS refit + geometry table consume. Because the same post-deform
vertices feed raster and ray tracing, **raster==RT geometry parity is structural** — not a hand-maintained
invariant. The arc grew this seam in four steps (D1 skinned RT parity → D2 raster reads it / retire VS
skinning → D3 generalize to static wind-deformable meshes → D4 per-entity wind authoring + richer model) and
landed procedural vertex wind on top, RT-correct by construction.

The scope is deliberately **bounded RT-correct hero deformables** (Bhaal-Temple banners), not forests. Research
(2026-06-16) confirmed cheap RT-correct *per-instance* wind does not exist in production: AAA does wind in the
material/vertex shader (per-instance world-position phase, zero memory) but it is raster-only — RT shows static
foliage; RT-correct animated geometry needs a per-instance BLAS, affordable only for a bounded set. The seam is
the right tool for the bounded case; a cheap raster-only forest VS-wind path is a separate future effort.

---

## Efforts

| Effort | What landed | Key commits |
|---|---|---|
| **D1 — skinned RT parity** | The deformed buffer carries post-skin **normals + tangents** (not just positions); the RT geometry table reads them so RT hits shade with the deformed TBN, matching raster. | `c2168220` |
| **D2 — raster deformation seam** | Raster VS reads the deformed buffer by `gl_VertexIndex` (empty vertex input, no bone deref — the `_skinned` shaders become *"deformed"* shaders). Buffer **double-buffered** (curr/prev) for motion vectors; deformed-buffer BDAs ride `GPUObjectData`; deform pass moved to the graphics queue at frame start; `skinning.comp` reads the `SkinnedVertex` VB directly (the tight-packed "skin input" buffer retired). VS-LBS skinning deleted. | `99a96f19` `cb6af576` `69e4c867` `a01be82a` `ae386615` `0c931529` |
| **D3 — static-deformable** | Static meshes opt into the seam (`MarkDeformable` import → `MeshData::IsDeformable`, model artifact V4→V5 + asset self-heal). New `deform.comp` (global object-space wind: height-scaled main bend + per-vertex detail, Crysis GPU Gems 3). Three-flag model `isSkinned`/`isDeformable`/`isDeformed`; the skinned BLAS factory renamed deformable. `WindSettings` + RenderPanel + ModelViewer checkbox. | `1b079cf2` `8e884f60` `55da7a39` `4e101c0c` `8f228302` `2b632e10` `a282f978` |
| **D4 — per-entity wind authoring** | Global wind FIELD split from per-entity RESPONSE. `WindSettings` gains gust envelope + in-shader turbulence; direction reinterpreted **world-space** (transformed to object space per instance). New `Component::Wind` (strength/gust/detail multipliers, phase offset, direction override) captured into the snapshot + folded into the dispatch. Inspector drawer (undo + copy/paste) + scene serialization. | `2afc887c` `05b5e6f2` `bb3ad362` `f303493a` |

---

## Architectural decisions

### One deformed buffer, two compute writers, three flags
`skinning.comp` (bones) and `deform.comp` (wind) write the identical 13-float interleaved `Vertex` output, so
a deformable mesh reuses the existing deformed pipelines + motion-vector path + BLAS refit + geometry table
wholesale. The three-flag model keeps the concepts distinct: `isSkinned` (has bones; also drives the VS-LBS
**selectionMask** pass, which hard-expects the 84 B skinned VB), `isDeformable` (static opt-in, 52 B `Vertex`),
`isDeformed = isSkinned || isDeformable` (reads the deformed buffer). Folding `isDeformable` into `isSkinned`
would misroute static-deformable meshes into the VS-LBS selectionMask pipeline (84 B layout mismatch + OOB bone
reads) — so the deformed raster loops switch to `isDeformed` while selectionMask stays on `isSkinned`.

### Global wind FIELD + per-entity RESPONSE (mirrors SpeedTree)
`WindSettings` is the global field (world-space direction, strength, main/detail bend, frequency, gust
amplitude envelope, turbulence). `Component::Wind` is the per-entity response (strength/gust/detail
multipliers, phase offset, optional direction override). An entity with no `Wind` component responds fully to
the global field (multipliers default 1) — marking a mesh deformable needs zero per-entity setup. Gust is a
global phenomenon (one envelope sweeps the scene); per-entity reactivity rides multipliers.

### World-space wind direction is a per-instance object-space transform
The deform is object-space, but wind blows in a world direction. The dispatch transforms the world dir into
each mesh's object space via `Math::Inverse(Mat3(worldMatrix))` — the plain inverse of the linear part, since
a wind direction is a **contravariant flow vector** (NOT the inverse-transpose normal matrix used for normals).
A length guard `(olen > 1e-5f && olen < 1e18f)` rejects both inf (singular/zero-scale matrix) and nan → no
bend. Correct for the single-instance hero case; rotated banners bend the same world direction.

### Empty descriptor-set compute — turbulence is in-shader noise
`deform.comp` has EMPTY descriptor-set layouts; all inputs ride a 72 B `DeformPC` push constant + buffer device
addresses (no Set 0, no texture). Turbulence is two extra `smoothTriangleWave` octaves at distinct
spatial/temporal frequencies, computed in-shader — keeping the empty-descriptor contract (no noise-texture
binding). Wind animation is **stateless** (driven by `Time::GetTime()` + per-entity phase offset; no
accumulation/history), so it composes trivially with the frozen-snapshot frame model.

### The per-asset wall (last-writer-wins)
The deformed buffer is per-mesh-asset, so all instances of a mesh deform in lockstep. The dispatch iterates
per-instance and writes the shared buffer, so per-entity wind params are last-writer-wins when several entities
share a deformable mesh — meaningful only for distinct (single-instance) hero meshes. Per-instance deformation
would need per-instance buffers + BLAS (the expensive RT-correct per-instance axis), out of scope.

---

## Files touched (D4)

**Engine (Luth.lib):** new `scene/components/Wind.h`; `renderer/settings/WindSettings.h` (gust/turbulence +
world-space dir), `renderer/subsystems/SkinningSubsystem.cpp` (72 B `DeformPC`, per-instance world→object dir +
per-entity fold), `core/RenderSnapshot.{h,cpp}` (inline wind capture), `scene/Components.h`,
`scene/SceneSerializer.cpp` (Wind save/load). **Shader:** `assets/shaders/deform.comp` (gust + turbulence,
grown push block). **Editor (Luthien.lib):** new `inspectors/component_drawers/WindDrawer.cpp` +
registration; `panels/RenderPanel.cpp` (gust/turbulence rows). **Docs:** `core/Version.h` (3.3.0),
`arch/rendering-pipeline.md` (seam note), `ROADMAP.md`, this file.

(D1–D3 file lists are in the per-effort commits; the seam machinery lives in `SkinningSubsystem`,
`VulkanAccelerationStructure`, `TlasBuilder`, `Model`/`Mesh`, `skinning.comp` + `deform.comp`.)

---

## Verification

`glslc --target-env=vulkan1.3 deform.comp` clean; `spirv-dis` confirms the 16-member push block matches the
72 B `DeformPC` (2×u64 BDAs + 14×4-byte, no pad, 8-aligned; `static_assert(sizeof==72)`). Full Debug x64 build
green throughout. Two independent adversarial reviewers (ABI/math + integration/edges) returned **SHIP**, no
blockers/majors — confirming the no-component path is byte-identical to prior global-only behavior, the
snapshot is frozen-safe, serialization is additive + round-trips, and the drawer commands mirror the FogVolume
pattern. User runtime smoke: a banner with a `Wind` component waves per its params; RT shadow/reflection track
the bend (raster==RT); gusts pulse; distinct meshes de-sync via phase offset; rotating a banner keeps wind in
the same world direction; old scenes load with full global response; a non-deformable scene is unchanged.

**Process deviation:** the new `WindDrawer.cpp` required a premake regen — `Luthien.vcxproj` lists source files
explicitly (premake-generated), it is NOT auto-globbed as the D4 plan assumed.

**Known limitations (deferred, not rework):** deformed normals pass through bind-pose (second-order shading;
recompute-from-bend-gradient is future); selectionMask outline shows bind-pose for deformable meshes (stays on
`isSkinned` for the 84 B-VB safety); wind doesn't combine with skinning (windblown skinned cloak); per-instance
deformation + cheap forest VS-wind are separate future efforts.
