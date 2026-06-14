# material-authoring (v3.2.8)

**Date:** 2026-06-14 · **Issue:** Part of [#157](https://github.com/Hekbas/Luth/issues/157) · **Series:** `slang-material` / Materials arc M1 · **Branch:** `feat/material-authoring`

## Summary

The M1-authoring closeout: three node-graph authoring gaps, batched onto one branch per the
size-tiered ceremony policy. **(A)** a graphed `Output.normal` was **inert** — every shading
consumer computed its own world normal and never read `mi.normal`. **(B)** the editor's material
preview (inspector footer + ProjectPanel thumbnail) rendered a fixed Lambert shader, ignoring the
graph. **(C7)** there was no node search / quick-add. The series invariant — **raster == RT** —
holds throughout.

Granular per-node undo (the second half of `node-ux`) was **deferred** after implementation
analysis (see Design decisions); coarse graph undo already works via the inspector debounce, so it
is not a regression.

## What shipped

### A — honor `mi.normal` across raster + RT (`feat(material): honor graphed mi.normal across raster + RT`)

`MaterialInputs.normal` is now a defined **tangent-space** normal, `[-1,1]`, identity `(0,0,1)`
(`material.slang`). A shared `ApplyTangentNormal(tn, T, B, N)` helper does the TBN transform;
`ApplyNormalMap` was refactored onto it (byte-identical). All three consumers — `pbr_shade.slang`
(opaque), `pbr_transparent_shading.slang` (transparent), `material_bindings_rt.slang` (RT) — now,
after computing their base `N` and before the facing flip, override it with the graphed normal when
the graph drives it: `if (any(mi.normal != (0,0,1))) N = ApplyTangentNormal(mi.normal, <TBN>)`. RT
is guarded by `g.hasTangent` (tangentless RT meshes keep `g.ns` — the same behaviour stock normal
maps already have). Codegen decodes a graph `TextureSample(Normal)` (`*2-1`) so routing a normal
map to `Output.normal` produces a proper signed tangent normal.

**Sentinel, not a flag bit:** identity `(0,0,1)` through any orthonormal TBN returns the vertex
normal exactly = today's non-normal-mapped output, so a false-negative is pixel-identical. raster==RT
holds because the same `EvalGraph_<hash>` body produces `mi.normal` on both tiers and the same
`ApplyTangentNormal` transforms it (differing only in TBN source — the established stock-map seam).
A flag would force C++/`GPUMaterialData` work + a desync surface for zero benefit. Pure shader +
codegen, no struct change → `MaterialLayoutGuard` untouched.

### B — lightweight graph-aware preview (`feat(material): emit a graph preview consumer per structure` + `feat(editor): graph-aware material preview`)

Codegen emits a third consumer flavour per structure — `thumbnail_graph_<hash>.slang` (beside the
opaque `pbr_graph_<hash>`) — that decodes via `EvalGraph_<hash><PreviewFetch>` then Lambert+ambient
over `mi.baseColor` + the graphed normal. It reads a **self-contained** per-call UBO
(`common/material_bindings_preview.slang`: `PreviewFetch` + `PreviewMaterialUBO`) and a new GLSL
`thumbnail_graph.vert` that delivers the tangent + UV1 the decode/normal need. `Material` carries a
`m_GraphPreviewShaderUUID` (mirroring `m_GraphShaderUUID`, shared per structure via `StructInfo`).

`ThumbnailPreviewScene` keeps its stock Lambert pipeline as the fallback and adds: a per-UUID graph
pipeline cache (`ResolvePreviewSpv` mirrors `GeometrySubsystem::ResolveFragSpv`), a two-set graph
layout `{ preview UBO @0, bindless @1 }`, host-visible preview UBOs (one per inspector ring slot +
one for the bake), and a shared `RecordPreviewDraws` that branches graph-vs-Lambert. Value edits
refill the UBO (no pipeline rebuild); a structural edit's new hash → new preview UUID → cache miss →
rebuild.

**B1 (self-contained), not B2 (bind the real Set 2):** the preview submits off the swapchain frame
loop (`RenderInspectorInternal` on a dedicated timeline; `BakeMeshInternal` via `ImmediateSubmit`),
MaterialSystem's Set 2 is per-frame transient, and the previewed material may be unregistered (a
brand-new graph in the inspector) — so binding the real material set is unsafe/garbage for the
live-authoring case. The per-slot UBO sync rides the inspector ring's existing timeline wait; the
bake is synchronous. Lambert-over-graph only — **not** a PBR pass. Material previews render the
static Sphere primitive, so only a static-stride graph pipeline is needed (no skinned variant).

### C7 — searchable node quick-add (`feat(editor): searchable node quick-add in the graph panel`)

Space over the canvas opens a filtered quick-add popup: type to filter the node types
(case-insensitive substring), Enter adds the first match, click adds any. Reuses `MakeNode`/
`NextNodeId`/`kTypes[]`; a shared `DrawNodePalette` helper.

## Design decisions / deviations

- **Part A sentinel (above).**
- **Preview approach B1, code-decided (above).** B2's "smaller" appeal assumed the preview already
  binds Set 2 — it binds only bindless — so B2 would be *larger* and fragile.
- **`Material::GPUMaterialData` is namespace-scope `Luth::`, not nested** — the preview UBO mirror
  uses the unqualified type.
- **Granular per-node undo (C8) deferred.** `Material::MarkDirty()` sets both `m_GpuDirty` and
  `m_NeedsSave`, and the MaterialEditor's 0.5s debounce pushes a `MaterialSnapshotCommand` on any
  `NeedsSave` — so a graph-panel per-node command would double-record alongside the inspector
  snapshot. Avoiding it needs a cross-panel debounce-suppress flag whose timing has real edge cases
  (interleaved graph + inspector edits within the window). Graph edits **already** get coarse undo
  via that debounce, so deferral is no regression — only the granularity waits for a `node-ux`
  follow-up that can iterate live. The user agreed to defer.
- **Effort batched onto one branch** per [[feedback_effort_ceremony_tiering]] (S/S-M siblings in one
  arc-phase). `node-breadth` (M, with the custom-Slang escape-node parity fork) stays its own effort.

## Deferred (known limitations)

- **Granular per-node/per-link undo** + **comments/frames** + **group-to-subgraph** → a `node-ux`
  follow-up slice (iterated with the app running).
- **Full-PBR preview** — Lambert+ambient only, by design.
- **`node-breadth`** — noise / UV transforms / fresnel / new inputs + the custom-Slang escape node.

## Files

- **Engine shaders:** `common/material.slang` (convention + `ApplyTangentNormal`), `common/pbr_shade.slang`, `common/pbr_transparent_shading.slang`, `common/material_bindings_rt.slang` (honor `mi.normal`), **new** `common/material_bindings_preview.slang` (`PreviewFetch` + UBO), **new** `thumbnail_graph.vert` (+ `.meta`)
- **Engine:** `renderer/material/MaterialGraphCodegen.cpp` (Normal decode + `EmitPreviewConsumer` + wiring), `renderer/material/Material.{h,cpp}` (`m_GraphPreviewShaderUUID`), `core/Version.h`
- **Editor:** `luthien/widgets/ThumbnailPreviewScene.cpp` (graph pipeline + UBO + `RecordPreviewDraws`), `luthien/panels/MaterialGraphPanel.cpp` (search/quick-add)
- **Docs:** `ROADMAP.md`, this history file

## Sub-tasks (commit order)

| # | Commit | Notes |
|---|--------|-------|
| A | `feat(material): honor graphed mi.normal across raster + RT` | helper + 3 consumers + codegen Normal decode |
| B (1) | `feat(material): emit a graph preview consumer per structure` | `material_bindings_preview` + `thumbnail_graph.vert` + `EmitPreviewConsumer` + `m_GraphPreviewShaderUUID` |
| B (2) | `feat(editor): graph-aware material preview` | `ThumbnailPreviewScene` graph pipeline + UBO + `RecordPreviewDraws` (+ vert `.meta`) |
| C7 | `feat(editor): searchable node quick-add in the graph panel` | Space quick-add + filter |

## Verification

- Each commit built Debug x64 clean — only the documented pre-existing warnings (C4267
  size-narrowing, C4996 `getenv`/`strncpy`/`sprintf`, C4244 chrono, `vulkan-1.lib` LNK4006).
- **Offline slangc** (engine session flags `-profile spirv_1_5 -fp-mode precise
  -matrix-layout-column-major`): all three Part-A consumer entries (`pbr` / `pbr_transparent` /
  `pbr_oit_store`) + an RT megakernel (`path_trace`, `restir_gi_initial`) emit SPIR-V; the new
  `thumbnail_graph.vert` (glslc) + a hand-authored `thumbnail_graph_<hash>` consumer (against a
  project registry override, exercising `PreviewFetch` + `PreviewVaryings` + `ApplyTangentNormal` +
  `EvalGraph<PreviewFetch>`) emit SPIR-V.
- **Runtime smoke (user):** a `TextureSample(Normal) → Output.normal` graph perturbs the normal in
  the viewport (ShadeMode=Normals) and matches in Path-Trace (raster==RT, tangentless RT falls back
  to `g.ns`); the inspector footer + ProjectPanel thumbnail show the graph's baseColor + graphed
  normal under Lambert (value drag = no pipeline rebuild; structural edit rebuilds; non-graph /
  mesh previews unchanged); Space opens the node quick-add, typing filters, Enter adds.
