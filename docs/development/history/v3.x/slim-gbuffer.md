# rt-renderer.2-slim-gbuffer — slim-gbuffer

**Date:** 2026-05-22
**Commits:** 10 (on `feat/slim-gbuffer`)
**Issue:** [#129](https://github.com/Hekbas/Luth/issues/129)
**Umbrella:** [#127](https://github.com/Hekbas/Luth/issues/127)
**Series:** `rt-renderer`, second effort. Mode A series-coalesced — `Version.h` PATCH bump to `v3.0.1`, tag-only, no Release.

---

## Overview

Second sub-effort of the `rt-renderer` v3.0.x arc. New `SlimGBufferPass` after `DepthPrepass` writes per-pixel world-space normal (RG16F, octahedral) + roughness (R8) + motion vectors (RG16F, NDC delta) + material ID (R16U) at viewport resolution. Reads prepass depth with `EQUAL` test. Foundation for:

- **A.5 TAA** — motion vectors for history reprojection (Karis14)
- **Phase B/C RT denoising** — normal + roughness for spatial/temporal filtering
- **Phase D RT reflections** — normal for stochastic GGX importance sampling

Three genuinely new pieces of state for the motion-vector path:

- **Previous-frame view-projection** in `GlobalUniforms` (Set 0). Per-view storage on `ViewResources::prevViewProj` after a multi-view contamination bug surfaced in smoke (see "Bugs caught" below).
- **Previous-frame model matrix** per draw in `GPUObjectData` (Set 5). Render-side `m_PrevModelByEntity` cache on `GeometrySubsystem` — pipeline-safe (gameplay never touches render-frozen state); atomic-replace at end of `BuildGPUObjectBuffer` per Option B of the plan.
- **Previous-frame bone matrices** for skinned meshes. Dual-region `BoneMatrixBuffer` SSBO via sibling `m_PrevCpuScratch` (2 MB) + 2× GPU region per frame. Static offset `prevBoneOffset = boneOffset + 32768` reuses the existing `_pad` slot in `GPUObjectData` — zero net struct growth from the dual-buffer mechanism.

User-locked at Phase 3: verification UX bundles both dedicated frame-debugger preview decoders AND four `ShadeMode` toggles. Both ship. The frame-debugger decodes octahedral normal + integer matID through purpose-built shaders. The ShadeMode toggle adds a `SlimVizPass` that blits the selected attachment to LDR in real-time without requiring a capture.

Plan-mode discipline held: no new allocator, no new sync primitive, no new descriptor set, no Set 0/1/4/5 reshape. Two new formats (`RG16_Float`, `R16_Uint`) threaded additively through the existing `Texture::TextureFormat` + `RG::TextureFormat` switches.

---

## Sub-tasks

| # | What landed | Commit |
|---|---|---|
| 1 | **`prevViewProjection` + `prevModel` plumbing.** `GlobalUniforms` gains `prevViewProjection` (+64 B); `GPUObjectData` gains `prevModel` (+64 B) and renames `_pad` → `prevBoneOffset` (struct grows 112 → 176 B, `static_assert` lockstep). `GlobalSubsystem::UpdateUBO` captures `m_CachedViewProj` into `prevViewProjection` before the existing store. `GeometrySubsystem` adds `m_PrevModelByEntity` (`unordered_map<entt::entity, Mat4>`); `BuildGPUObjectBuffer` resolves prev-model from cache, atomic-replaces from current snapshot at end of loop. 7 GPUObjectData GLSL blocks + 12 GlobalUniforms GLSL blocks updated. No consumer reads the new fields yet — engine renders identically. | [`d245a71`](../../../../commit/d245a71) |
| 2 | **`BoneMatrixBuffer` dual-buffer.** Sibling `m_PrevCpuScratch` (2 MB, identity-init). `Update()` allocates 2× GPU region (4 MB); first half = `m_CpuScratch` (current bones), second half = `m_PrevCpuScratch` (frame N-1 snapshot). End-of-`Update()`: `memcpy(m_PrevCpuScratch, m_CpuScratch, BUFFER_SIZE)` snapshots for next frame. `BuildGPUObjectBuffer` sets `obj.prevBoneOffset = boneOffset + PREV_BLOCK_OFFSET` (= 32768). Single SSBO, single binding, no Set 4 reshape. GPU tagged heap absorbs the +2 MB/frame size growth via the existing large-one-shot path. | [`eba42ad`](../../../../commit/eba42ad) |
| 3 | **G-buffer targets + R16U/RG16F formats.** `Texture::TextureFormat::R16_Uint` + integer-sampler branch + format-string + `VkFormat` mapping in `VulkanTexture.cpp`. `RG::TextureFormat::RG16_Float` + `R16_Uint`. Switch-case threading in `RenderGraph.cpp`, `RenderResourceCache.cpp`, `FrameDebugger.cpp`. `FrameTargets` gains `m_SlimNormal` / `m_SlimRoughness` / `m_SlimMotion` / `m_SlimMaterialID` + accessors + `Allocate`/`Resize` lines. `RenderPipeline::RegisterNamedTextures` exposes the 4 targets. Targets allocate/resize cleanly; debugger panel lists them but no pass writes them yet. | [`630da96`](../../../../commit/630da96) |
| 4 | **`SlimGBufferPass` + pipelines + shaders.** `GeometrySubsystem::AddSlimGBufferPass` mirrors `AddDepthPrepass` shape: 4 color attachments (`LOAD_OP_CLEAR` w/ encoded up-vector normal default), depth attachment via `WriteDepth(LOAD, STORE)` with `EQUAL` test + `depthWrite = false`. Two pipelines (skinned + non-skinned), full PBR vertex stride. Three new shader files: `slim_gbuffer.vert`, `slim_gbuffer_skinned.vert`, `slim_gbuffer.frag` — octahedral encode (Karis14), NDC motion delta, R16U matID = `v_MaterialIndex & 0xFFFFu`. Opaque-only iteration; cutout coverage deferred (cutouts fail depth-EQUAL — prepass clears to 1.0). `RenderPipeline::Execute` wires `AddSlimGBufferPass` between `AddDepthPrepass` and `AddPrefilterPass`; 4 tracked RT names registered for frame-debugger archive. | [`ad2c50c`](../../../../commit/ad2c50c) |
| 5 | **Pipeline hot-reload + shutdown.** `OnShaderReloaded` dispatch entries for `slim_gbuffer.{vert,frag,_skinned.vert}` reuse the existing `deferGfx` `PushDeletion` lambda. Shader edits trigger asynchronous pipeline rebuild without device-wait. | [`15fb570`](../../../../commit/15fb570) |
| 6 | **Frame-debugger preview decoders.** New `debugSlimDecode.frag` (handles SlimNormal/SlimMotion/SlimRoughness via mode push constant) + `debugSlimMatID.frag` (`usampler2D` for R16_UINT integer texture, prime-fraction palette matching `pbr.frag`'s EntityID viz). `FrameDebugger` gains `slimDecodePipeline` + `slimMatIDPipeline`; both share the existing `descSet` layout. `FrameDebuggerContext::BlitArchivedSlimToPreview(archiveIdx, mode, scale)` mirrors `BlitArchivedDepthToPreview` shape — separate `m_SlimPreviewImage` per-archive cache. Forwarders through `RenderPipeline` + `RenderingSystem`. `FrameDebuggerPanel` detects `Slim*` archive names by string match, routes to the slim blit path, exposes a motion-scale `ImGuiSliderFloat` (1.0–200.0, logarithmic) in the panel. | [`055d996`](../../../../commit/055d996) |
| 7 | **`ShadeMode` viz toggles.** Enum +4: `SlimNormal`, `SlimRoughness`, `SlimMotion`, `SlimMaterialID`. New `slim_viz.frag` (single shader, mode push constant, 4 sampler bindings + 1 `usampler2D` for matID). `PostProcessSubsystem::AddSlimVizPass(rg, ldrInput, slimGB, mode, scale)` runs after `AddCompositePass` when `ShadeMode` is one of the slim values — blits the decoded attachment to LDR, bypassing tonemap. Per-view `slimVizDescSet` allocated in `ViewResources` (4 stable sampler bindings written once per resize). `ScenePanel` Debug split menu gains a "Slim G-buffer" subsection with 4 radio buttons. | [`71cc0a7`](../../../../commit/71cc0a7) |
| 8 | **Validation fixes from smoke.** Two bugs caught by Vulkan validation on the live viz: (a) VUID 04553 — `R16_UINT` matID sampled through the `LINEAR` `m_Sampler`; integer formats lack `SAMPLED_IMAGE_FILTER_LINEAR_BIT`. Added `m_NearestSampler` to `PostProcessSubsystem` and `samplerNearest` to `FrameDebugger`, bound only to the matID slot. (b) VUID 01197 — `AddSlimVizPass` re-imported the slim attachments and LDR via `rg.ImportResource`, creating aliased RG resource nodes the barrier solver couldn't reconcile. Refactored to thread `SlimGBufferOutput` from the producer and use `builder.Write(ldrInput)` on the handle Composite returned. | [`b48192c`](../../../../commit/b48192c) |
| 9 | **Per-view prev-VP.** Static scene + still camera still showed saturated motion vectors after (8). Root cause: `GlobalSubsystem::m_CachedViewProj` was a single global, so when `GamePanel` queued its view and both `RecordView(gameView)` + `RecordView(sceneView)` called `UpdateUBO` in the same render frame, each view saw the other view's current VP as its "previous". Moved prev-VP to per-view storage on `ViewResources::prevViewProj`. `m_CachedViewProj` stays for its existing role (frustum cull + frame debugger within the same view's frame). | [`dc2400c`](../../../../commit/dc2400c) |
| 10 | **Wrap-up.** This history file. `ROADMAP.md` row → done. `arch/rendering-pipeline.md` updated with slim G-buffer + post-arc target-state notes. `Version.h` 3.0.0 → 3.0.1. `--no-ff` merge into `main` + `v3.0.1` tag. Mode A — tag-only, no Release. | this commit |

---

## Bugs caught during smoke testing

The pass shipped clean on the first build pass, but smoke testing surfaced three bugs in sequence that the architectural Phase 3 validation didn't catch. All fixes preserved as separate commits for bisect granularity:

1. **VUID 04553 — `R16_UINT` matID sampled through linear sampler.** Integer formats don't carry `SAMPLED_IMAGE_FILTER_LINEAR_BIT` per Vulkan spec. The slim-viz descriptor set was binding `m_Sampler` (LINEAR) for all 4 attachments; for binding 3 (matID), Vulkan requires a NEAREST sampler. Fix: dedicated `m_NearestSampler` (PostProcessSubsystem) + `samplerNearest` (FrameDebugger), bound only to the matID slot. Commit (8).

2. **VUID 01197 — RG layout-transition mismatch.** `AddSlimVizPass` was creating a *new* RG resource node for each of its 5 attachments (LDR + 4 slim) by calling `rg.ImportResource(...)`. Each `ImportResource` produces a fresh `ResourceNode` with `initialState = ShaderResource`; the actual GPU layout for these VkImages was tracked by *other* nodes (the producer-side ones from `AddSlimGBufferPass` + `AddCompositePass`). The RG's barrier solver emitted transitions assuming `oldLayout = ShaderResource`, but the actual current layout was `ColorAttachment` — validation correctly flagged the mismatch. Fix: thread the producer-side `SlimGBufferOutput` through to `AddSlimVizPass`; reuse handles directly via `builder.Read(slimGB.normal)` etc., and `builder.Write(ldrInput)` for LDR. Commit (8).

3. **Per-view prev-VP contamination.** Static scene with still camera + paused animation still showed saturated motion. The diagnostic (replacing `slim_viz.frag` mode 2 with a signed-motion-to-grayscale-tint visualization) revealed that motion *was* genuinely large for static geometry. Tracing back: `GlobalSubsystem::m_CachedViewProj` is a single member shared across all views. When `GamePanel` queued a view, `RecordView(gameView)` ran first and stored Game's VP into the global; then `RecordView(sceneView)` ran and read the global as its "previous" — which was actually Game's *current* VP. Cross-contamination produced huge prev-VP deltas → saturated motion on static geometry. Fix: per-view `prevViewProj` on `ViewResources`. Each `UpdateUBO` call reads/writes the current view's cache slot. `m_CachedViewProj` retained for its other role (per-view cull frustum within a single view's render). Commit (9).

The third bug is the kind that's hard to catch without a stress scenario (single-view Scene panel renders correctly; the bug only surfaces with Scene + Game both open, OR any queued view scenario). Adding to the V1-V6 hazard analogy class: "any per-view render state shared via global storage breaks under multi-view rendering." Worth folding into `arch/rendering-pipeline.md` as a documented hazard for future per-view state additions.

---

## Architectural decisions

### Skinned dual-buffer via doubled SSBO, not via Set 4 reshape

`BoneMatrixBuffer` already owned Set 4 binding 0 as an unbounded `mat4 bones[]` SSBO. Two paths for the previous-bones storage:

- **Add Set 4 binding 1** = previous bones. Cleaner conceptually but touches descriptor layouts and per-view bind logic.
- **Double the existing region**; current bones at offsets `[0, N)`, previous at `[N, 2N)`. `prevBoneOffset = boneOffset + N` resolves both halves through the same binding.

The doubled-region won. Single binding, no Set 4 reshape, the dual-buffer is fully internal to `BoneMatrixBuffer`. `GPUObjectData::_pad` repurposed as `prevBoneOffset` (zero net struct growth). The 4 MB GPU allocation routes through `GPUTaggedPageAllocator::AllocateLargeTagged` exactly like the previous single-region case — no allocator surface changes.

Skinned previous-frame bone matrices was a user-included scope item (Phase 3) — the alternative was the "current-bones approximation for prev" path documented as a known limitation in the original plan. The full dual-buffer ships in this effort; skinned motion vectors will produce correct deltas for character bone animation, validating against the foundation A.5 TAA + Phase B/C denoisers need.

### Slim-viz reuses Composite's LDR handle, not a re-import

First-pass `AddSlimVizPass` re-imported LDR with `ResourceState::ShaderResource` (matching the pattern `AddCompositePass` uses for its own output). The pattern works for Composite because Composite is the first pass to write LDR in the frame; the RG's UNDEFINED→ColorAttachment transition is a valid Vulkan transition regardless of the actual prior layout. SlimVizPass is *not* the first writer — Composite wrote LDR earlier in the frame, leaving it in `COLOR_ATTACHMENT_OPTIMAL`. SlimVizPass's re-import created a fresh node with `initialState = ShaderResource`, and the RG emitted a `ShaderResource → ColorAttachment` barrier whose `oldLayout` didn't match the actual current layout. VUID 01197.

The correct pattern: when a pass writes a target that an upstream pass already wrote, take the upstream's handle as a parameter and use `builder.Write(handle, ...)` directly. The RG's barrier solver tracks state on the *same node* and emits a correct `ColorAttachment → ColorAttachment` exec-barrier (WAW). Same lesson applies to the slim attachments — `SlimGBufferOutput` is now threaded through.

This is a general RG hazard worth surfacing in `arch/rendering-pipeline.md`: re-importing a VkImage that another pass in the same frame already imported aliases the same physical resource onto two RG nodes the barrier solver can't reconcile.

### Per-view prev-VP storage on ViewResources, not a per-view subsystem state

`GlobalSubsystem` is per-pipeline, not per-view. Its `m_CachedViewProj` is fine for read/write within one view's render (write during `UpdateUBO`, read during `Execute` for frustum cull / debugger). Across views in the same frame, the global gets stomped.

Two ways to keep prev-VP per-view:

- **Per-view map on GlobalSubsystem** (`unordered_map<ViewID, Mat4>`). Pollutes the subsystem with view bookkeeping that doesn't fit its role.
- **Field on `ViewResources`.** Each view already has a `ViewResources` instance with descriptor sets + bloom textures. Adding `Mat4 prevViewProj{ 1.0f }` is one field, identity-init mirrors the bootstrap behavior of every TAA pipeline.

The `ViewResources` path won. `UpdateUBO` reads `vr->prevViewProj` for `ubo.prevViewProjection`, writes the current VP back. `m_CachedViewProj` keeps its existing role unchanged.

### Cutout coverage deferred — opaque-only depth-EQUAL

`DepthPrepass` writes opaque depth with `LOAD_OP_CLEAR` to 1.0. Cutout fragments don't reach the prepass-written depth (cutouts depth is written in `GeometryPass`, which runs *after* SlimGBufferPass). SlimGBufferPass with `depthCompareOp = EQUAL` would discard cutout fragments outright (their computed depth < 1.0 ≠ the cleared 1.0).

The trade-off: match `DepthPrepass`'s opaque-only iteration and accept that cutout pixels show clear values in slim G-buffer outputs. Bhaal Temple is mostly opaque so the foliage / cutout-card case isn't a verification blocker. Documented as a deferred follow-up; the candidate fix when it lands is either extending `DepthPrepass` to include cutout (changes shadow behavior — requires its own analysis) or adding a separate `LESS_OR_EQUAL` cutout slim G-buffer pass.

### Two new RG TextureFormat values added, not a generic "format param"

`RG::TextureFormat` is an enum with a handful of values, each switch-cased to `VkFormat` in three sites (`RenderGraph::GetVkFormat`, `RenderResourceCache::format`, `FrameDebugger::ToVkFormat`). Two options for adding `RG16F` + `R16U`:

- **Replace the enum with a `VkFormat` passthrough.** Eliminates the switch threading but loses the RG-specific aliasing the enum enables (e.g., the engine could decide to upgrade `RG::TextureFormat::HDR_Color` from `RGBA16_Float` → `RGBA32_Float` at one site, propagating to all consumers).
- **Add two enum values, thread through three switches.** Mechanical, additive, preserves the abstraction.

Option 2 won. Three two-line additions to existing switches; no API surface change.

---

## Files touched

**Engine (Luth.lib):**
- [`scene/systems/RenderingSystem.h`](../../../luth/source/luth/scene/systems/RenderingSystem.h) — `GlobalUniforms` + `prevViewProjection`; `ShadeMode` enum + 4 slim entries; `SlimGBufferOutput` struct
- [`scene/systems/RenderingSystem.cpp`](../../../luth/source/luth/scene/systems/RenderingSystem.cpp) — `BlitArchivedSlimToPreview` + slim preview accessors
- [`renderer/draw/DrawCommand.h`](../../../luth/source/luth/renderer/draw/DrawCommand.h) — `GPUObjectData` + `prevModel`, `_pad` → `prevBoneOffset`, `static_assert(sizeof == 176)`
- [`renderer/subsystems/GlobalSubsystem.cpp`](../../../luth/source/luth/renderer/subsystems/GlobalSubsystem.cpp) — per-view prev-VP capture via `vr->prevViewProj`
- [`renderer/subsystems/GeometrySubsystem.{h,cpp}`](../../../luth/source/luth/renderer/subsystems/GeometrySubsystem.h) — `m_PrevModelByEntity` cache + slim-G-buffer pipelines + SPV blobs + `AddSlimGBufferPass` impl + hot-reload entries
- [`renderer/subsystems/PostProcessSubsystem.{h,cpp}`](../../../luth/source/luth/renderer/subsystems/PostProcessSubsystem.h) — `m_NearestSampler` + `m_SlimVizPipeline` + `m_SlimVizDescSetLayout` + `AddSlimVizPass` + descriptor write
- [`renderer/resources/BoneMatrixBuffer.{h,cpp}`](../../../luth/source/luth/renderer/resources/BoneMatrixBuffer.h) — `m_PrevCpuScratch` + dual-region GPU upload + end-of-Update snapshot + `PREV_BLOCK_OFFSET` constant
- [`renderer/resources/Texture.h`](../../../luth/source/luth/renderer/resources/Texture.h) — `TextureFormat::R16_Uint`
- [`renderer/backend/vulkan/VulkanTexture.cpp`](../../../luth/source/luth/renderer/backend/vulkan/VulkanTexture.cpp) — `R16_Uint → VK_FORMAT_R16_UINT` mapping + integer-sampler branch + format-string case
- [`renderer/FrameTargets.{h,cpp}`](../../../luth/source/luth/renderer/FrameTargets.h) — 4 new slim members + accessors + `Allocate`/`Resize`
- [`renderer/FrameDebugger.{h,cpp}`](../../../luth/source/luth/renderer/FrameDebugger.h) — `slimDecodePipeline` + `slimMatIDPipeline` + SPV blobs + `samplerNearest`
- [`renderer/debug/FrameDebuggerContext.{h,cpp}`](../../../luth/source/luth/renderer/debug/FrameDebuggerContext.h) — `BlitArchivedSlimToPreview` + slim preview texture + decoder pipeline build in `InitDebugBlitResources`
- [`renderer/RenderPipeline.{h,cpp}`](../../../luth/source/luth/renderer/RenderPipeline.h) — `ViewResources::slimVizDescSet` + `prevViewProj`; `Execute` wires `AddSlimGBufferPass` + conditional `AddSlimVizPass`; forwarders for slim preview
- [`renderer/ViewResources.cpp`](../../../luth/source/luth/renderer/ViewResources.cpp) — `allocSingle(GetSlimVizDescSetLayout(), slimVizDescSet, ...)`
- [`renderer/rendergraph/RenderGraphResources.h`](../../../luth/source/luth/renderer/rendergraph/RenderGraphResources.h) — `RG::TextureFormat::RG16_Float`, `R16_Uint`
- [`renderer/rendergraph/RenderGraph.cpp`](../../../luth/source/luth/renderer/rendergraph/RenderGraph.cpp), [`RenderResourceCache.cpp`](../../../luth/source/luth/renderer/rendergraph/RenderResourceCache.cpp) — switch-case additions

**Shaders:**
- [`slim_gbuffer.vert`](../../../luth/assets/shaders/slim_gbuffer.vert) (NEW)
- [`slim_gbuffer_skinned.vert`](../../../luth/assets/shaders/slim_gbuffer_skinned.vert) (NEW)
- [`slim_gbuffer.frag`](../../../luth/assets/shaders/slim_gbuffer.frag) (NEW)
- [`debugSlimDecode.frag`](../../../luth/assets/shaders/debugSlimDecode.frag) (NEW)
- [`debugSlimMatID.frag`](../../../luth/assets/shaders/debugSlimMatID.frag) (NEW)
- [`slim_viz.frag`](../../../luth/assets/shaders/slim_viz.frag) (NEW)
- Mechanical `GlobalUniforms` + `GPUObjectData` block updates across 13 existing shaders (`pbr.{vert,frag}`, `pbr_skinned.vert`, `depthPrepass.{,_skinned.}vert`, `shadowDepth.{,_skinned.}vert`, `selectionMask.{,_skinned.}vert`, `skybox.{vert,frag}`, `grid.frag`, `gpu_cull.comp`)

**Editor (Luthien.lib):**
- [`panels/FrameDebuggerPanel.{h,cpp}`](../../../luthien/source/luthien/panels/FrameDebuggerPanel.h) — slim archive dispatch + motion scale slider
- [`panels/ScenePanel.cpp`](../../../luthien/source/luthien/panels/ScenePanel.cpp) — Debug split menu + 4 slim radio buttons

**Docs:**
- [`arch/rendering-pipeline.md`](../arch/rendering-pipeline.md) — `SlimGBufferPass` row + slim-viz pass + 2 new RG hazards documented (re-import aliasing, per-view state via global storage)
- [`epics/rt-renderer.md`](../epics/rt-renderer.md) — A.2 Progress Tracker row → done
- [`ROADMAP.md`](../../ROADMAP.md) — v3.0.1 row

---

## Verification

Build clean on every commit (0 errors, pre-existing warnings only). Smoke checklist run on `feat/slim-gbuffer` after commit (9):

- Bhaal Temple sample + animated character. ShadeMode → SlimNormal/SlimRoughness/SlimMotion/SlimMaterialID each renders correctly full-screen.
- Static scene with still camera + paused animation: motion ≈ 0 (background black via `abs(m)*scale` encoding).
- Camera-induced motion: motion vectors trace world-space-projected camera delta as expected.
- Skinned character with active animation: per-bone motion visible on moving limbs; uniform regions (torso) near zero.
- Frame debugger archives: `SlimNormal` decodes to RGB world-normal preview; `SlimMotion` shows R/G magnitude with scale slider; `SlimRoughness` raw grayscale; `SlimMaterialID` palette-hashed colors.
- Shader hot-reload: edit `slim_gbuffer.frag`, save → pipeline rebuilds asynchronously, next frame shows the change.
- Viewport resize: 4 slim targets recreate without VMA leaks.

Validation layer silent on the production path. The two VUIDs (04553, 01197) that fired during smoke are fixed in commits (8) and (9).
