# material-node-editor (v3.2.5)

**Date:** 2026-06-14 · **Issue:** Part of [#157](https://github.com/Hekbas/Luth/issues/157) · **Series:** `slang-material` Phases 4+5 (fused) · **Branch:** `feat/material-node-editor`

## Summary

A Blender-style **node-based material editor** whose graphs lower to generated Slang and render identically in raster and ray tracing. The scope was deliberately a **channel-routing** vocabulary (math / blend / mix / remap / texture-sample / constant nodes computing the fixed `MaterialInputs` channels), not the unbounded effect-layer of the original Phase 4 — so the authoring surface is the node editor (Phase 5), built on a minimal slice of Phase 4's composition substrate. Phases 4 and 5 were fused into one consumer-driven vertical slice: the node editor is the goal, so the codegen substrate was built *with* its real client rather than designed in a vacuum.

The load-bearing idea: a graph lowers to `EvalGraph<F : ITexFetch>` — the exact signature of `material.slang`'s stock `EvalMaterialChannels`, generic over the fetch policy. Because channel-routing math is **derivative-free**, the same generated body is valid in both tiers (raster `Sample` auto-mips, RT `SampleLevel(…,0)`), so **raster == RT holds by construction**, the same invariant the whole series is built around. The two tiers reach that body differently: raster binds a **per-material generated fragment shader** (one pipeline per graph), while the RT megakernels — one shared kernel over all materials — dispatch a **per-material variant index** through a generated registry.

## What shipped

### 1. Shared shading seam (S0)

`pbr.slang`'s post-decode body (cutout, normal-map, spec-AA, lighting, screen-space composites) extracted verbatim into `common/pbr_shade.slang::PbrShadeSurface`, with the shared `VIn`/`FOut`/Set-0/3 bindings. `pbr.slang` becomes a thin entry that decodes via `EvalMaterialChannels` then delegates. A generated `pbr_graph_<uuid>.slang` is the same entry with the decode swapped to the graph's `EvalGraph`, so per-material codegen never duplicates the 100-line shading path.

### 2. Per-material raster pipeline binding (S1)

`GeometrySubsystem::DrawBatch` previously bound one hardcoded `pbr` pipeline for every opaque/cutout draw. It now groups draws by the material's fragment-shader UUID (`Material::m_GraphShaderUUID`, an optional `"graph_shader"` `.mat` key; invalid = stock) and binds per-group via the existing UUID-keyed `PipelineManager`. `ResolveFragSpv` maps a graph UUID → its SPIR-V via a `ShaderLibrary` scan (cached). Non-graph materials key on the stock `pbr` UUID exactly as before — byte-identical.

### 3. Graph schema + persistence (S2)

`MaterialGraph` POD (`MatNode` / `MatLink`, a 9-type `MatNodeType` vocabulary) shared engine↔editor. `Material` holds it; it round-trips through an additive optional `"graph"` key in `Material::Serialize/Deserialize` (absent → plain material, so existing `.mat` files are untouched).

### 4. DAG → Slang codegen (S3)

`MaterialGraphCodegen` topo-sorts the graph (DFS post-order from the `Output` terminal, so unreachable nodes are dead-stripped) and emits one `float4` SSA local per node — every value is `float4` (scalars broadcast), which sidesteps type inference and keeps the `Output` channel extraction uniform (`.x` / `.xyz` / `.rgb`). The body starts from `EvalMaterialChannels` and overrides only the connected `Output` channels, so a partial graph degrades to the stock baseline. It writes the module + the `pbr_graph` consumer under `<project>/Library/Generated/shaders/`, compiles via `SlangCompiler::Compile` (no new Slang feature — plain monomorphized generics), registers in `ShaderLibrary`, and stores the resulting UUID on the material. A lazy once-per-material trigger in `EnsureMaterialRegistered` (game-stage, off the render hot path) covers load-time; the editor triggers it on edits.

### 5. Editor panel (S4) + authoring (S5)

`MaterialGraphPanel` (a `Panel`) renders the selected material's graph on the vendored ImGuizmo `GraphEditor` canvas via a `Delegate`. **Create Material Graph** lays down a starter graph (diffuse × warm tint → baseColor); right-click adds/deletes nodes (8 non-terminal types); a left param pane edits `Const`/`Remap` values and the `TextureSample` map slot; connect/disconnect/move work on the canvas. Structural and value edits re-emit the shader live (const values recompile on release, not per drag-frame). Undo/persistence ride the existing inspector debounce → `MaterialSnapshotCommand` (the graph is in the `.mat` JSON). The ImGuizmo `GraphEditor.cpp` (present but never built) was added to the imguizmo project with an `AddBezierCurve`→`AddBezierCubic` ImGui-version-compat patch.

### 6. RT-tier parity (S6)

The RT megakernels share one kernel over all materials, so they can't bind a per-material shader. Instead `material_bindings_rt.slang`'s `FetchHitSurface` decodes through `EvalGraphVariant(GraphVariant(m.flags), …)`, dispatching a per-material variant index packed into `GPUMaterialData.flags` bits 8-15 (zero struct-size change → `MaterialLayoutGuard` untouched). Codegen assigns each graph material a variant (capped at 16), generates a project `mat_graph_registry.slang` (a `switch` over uniquely-named `EvalGraph_<safe>` per material, shadowing the engine default on the search path), and recompiles the RT consumers against it via a main-thread `MainThreadPump` reload. Variant 0 (every non-graph material) resolves to the stock decode, so RT output is unchanged for them and the `SlangParityGuard` stays green.

## Design decisions / deviations

- **Phases 4+5 fused, channel-routing only (resequenced).** The ROADMAP had Phase 4 (effect layer) blocking Phase 5 (node editor). The user's interest is the authoring UX, so the effort delivered Phase 5 as a channel-routing graph over a *minimal* composition substrate, and deferred the full composable effect-layer (the original Phase 4). Phase 4 is no longer a blocker for Phase 5; it becomes a follow-up that the node editor will emit into.
- **Two tiers, two mechanisms — by necessity.** Raster keys pipelines per shader UUID, so a per-material generated fragment shader is natural. RT is a megakernel over all materials with no per-instance shader, so it needs a runtime variant dispatch. Both call the same `EvalGraph<F>` body; the split is in *how the body is reached*, and the derivative-free vocabulary keeps the body identical across tiers.
- **All-`float4` SSA, no type inference.** Channel routing over scalars+vectors is correct with broadcast (`float4(0.3)` × a color scales all channels); emitting everything as `float4` and extracting at the `Output` avoids a type system in the codegen for zero correctness cost on this vocabulary.
- **Baked-literal constants (with eyes open).** Graph constants are baked into the generated Slang, so a value edit recompiles the shader, and — critically for RT — every edit recompiles all ~6 RT megakernels (a brief authoring-time hitch; the *rendered* scene has no hitch). This also caps distinct RT graph materials at the variant ceiling. The fix — routing constants through per-material data — is the top follow-up (see Deferred); it was scoped out to keep this effort's surface bounded.
- **Default registry must not live in `common/`.** Slang resolves an `import` relative to the importing module's own folder *before* the search paths. `material_bindings_rt` lives in `common/`, so a default `mat_graph_registry` beside it always shadowed the project's generated override — RT silently decoded stock. The default was moved to its own `registry/` dir (search-path-last) so the project's `Library/Generated` copy (search-path-first) wins. This was the subtle bug that made S6 look broken at first smoke.

## Bugs fixed along the way

- **Panel crash on non-material selection.** `AssetManager::GetAsset<Material>` does not type-check; selecting a *model* reinterpreted its memory as a `Material` → `bad_alloc`, and the throw unwound past `ImGui::End()` → "Missing End()" hard crash. Fixed with an `AssetDatabase::GetMetadata(sel).Type == Material` guard (mirroring `InspectorPanel`) and an RAII `EndGuard` so the window stack stays balanced on any throw.
- **RT registry shadowing** (the import-resolution gotcha above) — graphs rendered correctly in raster but stock in reflections/GI/path-trace until the default registry was relocated out of `common/`.

## Deferred (known limitations)

- **Composable effect layer** — the original Phase 4 (stackable parallax/detail/decal/triplanar units). The node editor will emit into it when it lands.
- **Constants → per-material data** — removes recompile-on-edit + the RT megakernel-recompile hitch + the 16-variant cap + the per-edit raster pipeline-handle leak. Highest-value next step.
- **Transparent / OIT graph materials** — `TransparencySubsystem` binds its own fixed `pbr_transparent` / `pbr_oit_store` shaders and never consults the material's graph UUID, so a transparent material renders stock in the raster transparent pass (it still picks up its graph in RT via the shared `FetchHitSurface`, so it is currently raster/RT-inconsistent). Wiring it means generating transparent-flavoured consumer variants.
- **Node breadth** (noise / UV transforms / math breadth / **graphed normal maps** — `mi.normal` is currently inert because `PbrShadeSurface` computes the normal itself), **graph-aware sphere preview** (the inspector preview shows the stock material), node search / comments, granular per-node undo.

## Files

- **New (engine):** `renderer/material/MaterialGraph.h`, `renderer/material/MaterialGraphCodegen.{h,cpp}`, `assets/shaders/common/pbr_shade.slang`, `assets/shaders/registry/mat_graph_registry.slang`
- **New (editor):** `luthien/panels/MaterialGraphPanel.{h,cpp}`
- **Engine:** `assets/shaders/pbr.slang` (slim), `common/material.slang` (`GraphVariant`), `common/material_bindings_rt.slang` (variant dispatch), `renderer/material/Material.{h,cpp}` (graph + variant + pack), `renderer/draw/DrawCommand.h`, `renderer/DrawListBuilder.cpp`, `renderer/subsystems/GeometrySubsystem.{h,cpp}` (per-material binding + lazy codegen), `renderer/shader/SlangCompiler.cpp` (generated + registry search paths), `core/Version.h`
- **Editor:** `luthien/Editor.cpp` (register panel)
- **Vendor / build:** `extern/source/imguizmo/GraphEditor.cpp` (ImGui-compat patch), `extern/premake5-imguizmo.lua` (build GraphEditor)
- **Removed:** the S0 seam fixtures `common/mat_graph_test.slang`, `pbr_graph_test.slang` (superseded by real codegen)
- **Docs:** `ROADMAP.md`, `epics/slang-material.md` (tracker)

## Sub-tasks (commit order)

| # | Commit | Notes |
|---|--------|-------|
| S0 | `refactor(renderer): extract pbr shade body + graph seam` | `pbr_shade.slang` + `EvalGraph<F>` seam fixtures |
| S1 | `feat(renderer): per-material fragment pipeline binding` | DrawBatch grouping + `ResolveFragSpv` + `graph_shader` key |
| S2 | `feat(material): node-graph schema + .mat persistence` | `MaterialGraph` POD + serialize |
| S3 | `feat(material): node-graph to Slang channel-routing codegen` | DAG→SSA lowering + compile/register + lazy trigger |
| S4 | `feat(editor): node-based material graph panel` | `MaterialGraphPanel` + GraphEditor + create-graph (+ crash fix folded) |
| S5 | `feat(editor): node palette + per-node value editing` | right-click add/delete + param pane + recompile-on-edit |
| S6a | `feat(material): RT graph-variant dispatch seam` | `EvalGraphVariant` + flags 8-15 (byte-identical) |
| S6b | `feat(material): graph materials in RT via variant registry` | aggregator gen + main-thread megakernel reload |
| fix | `fix(material): move graph registry default out of common/` | import-resolution shadowing fix |

## Verification

Each sub-task built Debug x64 clean; the codegen output and all RT/transparent consumers were `slangc`-validated offline (including a deliberately-broken-override test that proved the project registry wins after the `common/` fix). Runtime-smoked by the user: a graph material renders its channel routing in the viewport, live-edits via add/connect/value, and — after the registry fix — shows identically in Path-Trace (raster == RT). Non-graph materials are byte-identical in both tiers. The `SlangParityGuard` SPIR-V gate is unaffected (variant 0 = stock decode).
