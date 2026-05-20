# v2.12.0 — async-compute-queue

**Date:** 2026-05-20
**Commits:** 10 (on `feat/async-compute-queue`)
**Issue:** [#126](https://github.com/Hekbas/Luth/issues/126)
**Series:** `renderer-pipeline`, first effort. Mode-B per-effort PATCH bump (MINOR bump at series start).

---

## Overview

Three-queue Vulkan foundation (graphics + async-compute + transfer) + per-view 3-submit topology + RenderGraph queue-aware dispatch with cross-queue-correct barrier emission. GTAO chain (3 compute passes) routed to async-compute as the first real exercise; `UploadContext` routed to a dedicated transfer queue for DMA-engine concurrency on discrete GPUs. CONCURRENT sharing-mode opt-in policy across cross-queue resources (SSBOs, storage images, sampled depth) preserves AMD DCC on color render targets. Single-family GPUs alias all three queues to graphics — same code path, no degenerate special-case.

This is the foundation that `forward-plus` ([#54](https://github.com/Hekbas/Luth/issues/54)) and `gpu-particles` ([#57](https://github.com/Hekbas/Luth/issues/57)) build on. Their compute work opts in via the 4-arg `AddComputePass<Data>(name, RG::QueueFamily::AsyncCompute, setup, execute)` overload — single-line API touch, no further infrastructure required.

Plan-mode validation surfaced five concrete gaps that became baked-in design changes rather than TODOs: cross-queue barrier stage-mask substitution (TOP_OF_PIPE on the reader's pre-barrier), explicit SceneDepth CONCURRENT opt-in, per-view 3-submit topology (vs. the original "one set of primaries per frame" sketch that would have raced on shared `m_ShadowMap`), GPUTimerPool single-period assertion, and FrameDebugger archive-sink queue-family hint. Two drive-by validation fixes (GTAO storage-image `descriptorBindingStorageImageUpdateAfterBind` device feature; BoneMatrix UAB) landed mid-effort when smoke-testing surfaced VUID hits that the per-view 3-submit overhead had exposed in latently-buggy paths.

---

## Sub-tasks

| # | What landed | Commit |
|---|---|---|
| 1 | **Queue family discovery + per-queue submit wrappers.** Priority-order family scan in `VulkanContext::CreateLogicalDevice` (graphics → COMPUTE_BIT-without-GRAPHICS → DMA-only TRANSFER → fallback aliases to graphics). One `VkDeviceQueueCreateInfo` per distinct family. `m_ComputeQueue`/`m_TransferQueue` + per-queue mutexes; `SubmitGraphics2`/`SubmitCompute2`/`SubmitTransfer2` wrappers around `vkQueueSubmit2` (kernel syscall — std::mutex correct per arch/memory.md). New `ApplyConcurrentSharing(VkBuffer/VkImage create-info)` helpers; size-1 deduped fallback to EXCLUSIVE. `timestampValidBits` compatibility assertion across in-use families (defers per-family-period support to future polish if hardware ever diverges). `LH_CORE_INFO` layout log. | [`bf82db6`](../../../../commit/bf82db6) |
| 2 | **Per-queue command pools + per-frame timeline ring caches.** `VulkanBackend` gains `m_ComputeCommandAllocatorPools` ring, `m_ComputePrimaryCommandPool` + compute primary cmds, graphics-B primary cmds, `m_ComputeTimeline`, and `m_LastGraphicsValuePerFrame[]` / `m_LastComputeValuePerFrame[]` ring caches. New `QueueRecorders { gA, compute, gB }` struct in `luth/renderer/QueueRecorders.h` threads through `Renderer::BeginPrimaryCmd` / `RecordGraph` / `EndPrimaryCmdAndSubmit`; `RenderPipeline::Execute` + `RenderingSystem::RecordView` take it as well. Empty primaries are valid no-op submits. Engine behavior unchanged — per-pass routing comes in sub-task 3. | [`41fbc64`](../../../../commit/41fbc64) |
| 3a | **Per-view 3-submit topology.** Cmd-buffer rings extended to per-view × per-frame (`std::array<std::array<VkCommandBuffer, MAX_VIEWS_PER_FRAME>, MAX_FRAMES_IN_FLIGHT>` = 12 cmds per queue stream). New `RenderBackend::SubmitView(frameIndex, viewSlot, recorders, hasComputeWork, isLastView)` replaces single-view `SubmitFrame` — implements gA → compute → gB per view with cross-queue semaphore waits; first view's gA waits `imageAvailable@COLOR_ATTACHMENT_OUTPUT`, subsequent views' gA waits the previous view's gB signal at `EARLY_FRAGMENT_TESTS_BIT` (replaces the inline `InsertInterViewBarrier` — same stage relationship, deleted). `AcquireImage` predicate uses the per-frame ring caches (`0` sentinel skips compute wait); `FreeTag(N-2)` cadence preserved. `m_CurrentFrameLastComputeValue` accumulates across views within a frame so the ring cache captures the final value even when only the first view has compute work. | [`57aeb45`](../../../../commit/57aeb45) |
| 3b | **RG queue routing + cross-queue barrier rule + FrameDebugger queue hint.** `RG::QueueFamily { Graphics, AsyncCompute }` enum + `PassNode::queueFamily` field; new 4-arg `AddComputePass` overload (3-arg keeps Graphics default — Cull / shadow cull / pre-existing compute passes unchanged). `RG::Execute(QueueRecorders, timers) → bool hasComputeWork`; Phase 2 dispatches per pass — AsyncCompute → `recorders.compute`, Graphics → `recorders.gA` before first AsyncCompute pass, `recorders.gB` after. `SolveBarriers` tags `Barrier::crossQueueSrc` when `m_Passes[lastWriter].queueFamily != currentPass.queueFamily`; Execute substitutes `srcStage = TOP_OF_PIPE_BIT, srcAccess = NONE` on emission so the compute primary never emits graphics-only src stages (VUID-vkCmdPipelineBarrier2-srcStageMask). `IArchiveSink::OnPassExecuted` + `FrameDebugger` gain `QueueFamily` parameter; sink substitutes `COMPUTE_SHADER_BIT` on the post-copy `dstStageMask` when on the compute primary. `vkCmdCopyImage` itself is queue-agnostic per spec. | [`5e5b113`](../../../../commit/5e5b113) |
| — (drive-by) | **Drive-by validation fix: missing `descriptorBindingStorageImageUpdateAfterBind` device feature.** GTAOMain layout already declared UAB on its storage-image output binding (per the in-source invariant comment about VUID 03047), but `VulkanContext::CreateLogicalDevice`'s required-features chain only enabled UAB for sampled-image / storage-buffer / uniform-buffer. NVIDIA was silently permissive; validation correctly flagged the spec violation at `vkCreateDescriptorSetLayout`. Pre-existing — not introduced by the effort, but surfaced when smoke-testing 3a. | [`42ae589`](../../../../commit/42ae589) |
| — (drive-by) | **Drive-by validation fix: UAB on BoneMatrix descriptor (VUID 03047).** Per-frame slot cycling (game frame K writes slot K%3, render reads (K-1)%3) provides write/read isolation in steady state but breaks when the GPU falls behind enough that frame K+3's game-stage write hits a slot still referenced by frame K+1's pending cmd buffer — per-view 3-submit's 3·N submits/frame increased the average pending-queue depth enough to expose the latent race. `VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT` added to the binding flags + matching `UPDATE_AFTER_BIND_POOL` flags on layout + pool. Same pattern as GTAOMain's per-render-stage rewrites. | [`6f05bc8`](../../../../commit/6f05bc8) |
| 4 | **CONCURRENT sharing for cross-queue resources.** `GPUTaggedPageAllocator` 64 MB backings + large-one-shot allocations → CONCURRENT via `ApplyConcurrentSharing` (universal CPU→GPU data path for per-frame SSBOs/UBOs that forward-plus / gpu-particles will read cross-queue). `RenderResourceCache::GetTexture` / `GetBuffer` → CONCURRENT for RG transients. `VKTexture::CreateImage` → CONCURRENT iff `STORAGE_BIT` (compute outputs) **OR** `DEPTH_STENCIL_ATTACHMENT_BIT + SAMPLED_BIT` (depth sampled by compute — SceneDepth → GTAODepthPrefilter); color RTs stay EXCLUSIVE (preserves AMD DCC). UploadContext staging + vertex/index buffers stay EXCLUSIVE. Single-family GPUs collapse all of the above to EXCLUSIVE via the `ApplyConcurrentSharing` size-1 fallback. | [`7aa6f95`](../../../../commit/7aa6f95) |
| 5 | **Route GTAO chain to async-compute queue.** `AddPrefilterPass` / `AddMainPass` / `AddDenoisePass` pass `RG::QueueFamily::AsyncCompute` via the 4-arg `AddComputePass` overload. First multi-queue exercise end-to-end: RG dispatches GTAO×3 onto `recorders.compute`; cross-queue barriers handle SceneDepth (graphics-A → compute read via DepthPrepass → GTAODepthPrefilter) and `gtaoFinal` (compute → graphics-B fragment read via GTAODenoise → PBR Geometry sample) via TOP_OF_PIPE substitution on the reader's pre-barrier. Cull + shadow cull stay Graphics (default — tiny dispatches, cross-queue overhead dominates per AMD guidance). | [`fd01467`](../../../../commit/fd01467) |
| 5.5 (smoke fix) | **Non-primary LDR transition on gB primary.** The "transition LDR → SHADER_READ so the scene view's ImGui can sample it" barrier (for non-primary views, i.e., the game panel) was recording on `recorders.gA` after my sub-task 2 aliased `primaryCmd = recorders.gA`. gA runs before gB on the GPU timeline — the transition would precede PostProcess Composite's LDR write, leaving LDR in SHADER_READ_ONLY when the next frame's PostProcess pre-barrier expected COLOR_ATTACHMENT_OPTIMAL (VUID-vkCmdDraw-None-09600). Moved the barrier to `recorders.gB` so it runs as the last cmd in gB, after PostProcess + Outline finish writing LDR. Only surfaces with Scene + Game multi-view; single-Scene has `emitImGuiPass = true` so the transition path doesn't run. | [`cbcf9aa`](../../../../commit/cbcf9aa) |
| 6 | **UploadContext → dedicated transfer queue + new `arch/multi-queue.md`.** Parallel graphics-blit ring (RING_SIZE cmds, graphics-family pool) added alongside the transfer ring; `UploadBuffer` + `UploadImage` submit via `SubmitTransfer2` on the DMA-capable family (truly concurrent with frame rendering on discrete GPUs), `UploadImageMipped` via `SubmitGraphics2` (vkCmdBlitImage requires `VK_QUEUE_GRAPHICS_BIT` per VUID-vkCmdBlitImage-commandBuffer-cmdpool). Shared `m_UploadTimeline` — Vulkan timeline semaphores accept multi-queue signal; `DrainPendingBinds` polls `GetValue()` agnostic of which queue retired. `UploadImage` barriers converted to sync2; post-copy `dstStage = BOTTOM_OF_PIPE_BIT` (transfer-queue-compatible — the upload-fence wait chain supplies the graphics-side fragment-shader dependency). New `arch/multi-queue.md`: discovery + three timelines + per-view 3-submit topology + RG queue routing + cross-queue barrier rule + CONCURRENT policy + UploadContext dual ring + FrameDebugger queue-aware sink + GPUTimerPool single-period assumption + V1–V6 hazard composition + single-family fallback + validation expectations + smoke-test checklist + future extension points. | [`c87d960`](../../../../commit/c87d960) |
| W | **Wrap-up.** This history file. `Version.h` bump 2.11.1 → 2.12.0. ROADMAP table move (Planned → Completed). CLAUDE.md Current Progress + Next + Active series update. `--no-ff` merge + `v2.12.0` tag (Mode-A series start, tag-only). | this commit |

---

## Architectural decisions

### Per-view 3-submit topology (gA → compute → gB) over single-graphics-submit

Two designs were on the table when the plan was approved:
- **Option A** (planned originally): single set of {gA, compute, gB} primaries per frame; all views' cmds concatenated within. Inter-view ordering enforced by intra-cmd-buf barriers (same as v2.11's `InsertInterViewBarrier`).
- **Option B** (adopted): per-view 3-submit triplet. Each view gets its own {gA, compute, gB} cmd buffers and submits its own three-submit transaction. Inter-view ordering enforced by view K+1's gA submit waiting on view K's gB signal at `EARLY_FRAGMENT_TESTS_BIT` (same stage relationship as the legacy barrier).

Option A would race when views share mutable resources. Specifically: at any time only one queue submission runs per queue family on the GPU. With single-primary-per-type, gA submit runs everyone's gA cmds in order, then compute, then gB. For a shared `m_ShadowMap` (per-cascade shadow render output, written by view K's shadow pass and view K+1's shadow pass with different cascade fits), this means view 2's shadow write would run on the gA queue BEFORE view 1's PBR fragment read on the gB queue — overwriting the shadow data view 1's PBR expects.

The fix isn't to alias resources per-view (would require restructuring shadow/light gather to be per-camera, out of scope) — it's to keep per-view submits independent at the queue level. Option B does that; the per-view timeline waits sequence views correctly even though their submits hit the same three queues.

The cost is 3·N submits/frame instead of 1. Windows submit overhead measures ~10–50 µs per submit; for typical N=2 (Scene + Game panels) that's 60–300 µs/frame, dwarfed by the intra-frame compute-overlap gains. Empirically inconsequential.

### Cross-queue barriers via TOP_OF_PIPE substitution, not QFOT

`SolveBarriers` detects cross-queue handoffs by comparing `m_Passes[lastWriter].queueFamily` to the current pass's family. When they differ, the reader's pre-barrier is emitted with `srcStage = VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT` and `srcAccess = VK_ACCESS_2_NONE` — the cross-queue semaphore wait at submit time *is* the memory dependency per Vulkan spec (chapter 7.4.2 "Semaphore Signaling and Unsignaling": semaphore wait implicitly synchronizes prior writes available to the waiting submit's operations). The reader's barrier only needs to establish the layout transition + dst access scope.

The alternative — EXCLUSIVE + paired release/acquire `VkImageMemoryBarrier2` (QFOT) — would have required `SolveBarriers` to emit two coordinated barriers per cross-queue handoff (one on the writer's queue, one on the reader's), each with matching subresource ranges + access masks. Documented in `arch/multi-queue.md` as a future-polish path because: (1) NVIDIA driver guidance is explicit that `VkSharingMode` is ignored — CONCURRENT carries zero overhead; (2) AMD's only documented concern is DCC on color render targets, which the per-resource CONCURRENT opt-in preserves (color RTs stay EXCLUSIVE); (3) the barrier solver complication isn't justified until a workload surfaces where DCC on a cross-queue color RT actually matters (forward-plus and gpu-particles don't currently write color RTs from compute).

### CONCURRENT opt-in per call site, not inferred from usage flags

`VulkanContext::ApplyConcurrentSharing(VkBuffer/VkImage create-info)` is the helper; opt-in is per call site. Mistakes the per-resource policy could make:

- **Too broad:** apply CONCURRENT to every VKTexture. Disables DCC on AMD color RTs even though they're graphics-queue-only (SceneColor, LDR are never read on compute today).
- **Too narrow:** infer from `STORAGE_BIT` alone. Misses SceneDepth — it has `DEPTH_STENCIL_ATTACHMENT_BIT | SAMPLED_BIT | TRANSFER_SRC_BIT` (no `STORAGE_BIT`) but is sampled cross-queue by `GTAODepthPrefilter` via `ReadStorageImage` → SHADER_READ_ONLY (the `ReadStorageImage` API name is historical; it resolves to sampled-image binding when the source has SAMPLED_BIT).

The adopted rule: opt in when the call site *knows* the resource crosses queues. STORAGE_BIT auto-qualifies (compute outputs typically read on graphics-B). DEPTH_STENCIL_ATTACHMENT_BIT + SAMPLED_BIT auto-qualifies (depth typically sampled). All other VKTextures opt in explicitly via a future-added flag (none today — SceneColor and LDR are explicitly EXCLUSIVE to preserve DCC).

GPU tagged-heap backings are unconditionally CONCURRENT because they're the universal CPU→GPU data path — every per-frame SSBO and UBO lives there, and forward-plus / gpu-particles will read them from compute. RG transients are unconditionally CONCURRENT because they may cross queues in any direction depending on which passes opted into AsyncCompute.

### Drive-by validation fixes mid-effort, not separate cleanup

Two validation errors surfaced during the sub-task 3 smoke test (`descriptorBindingStorageImageUpdateAfterBind` missing; BoneMatrix UAB). Both were pre-existing — neither was introduced by the effort. The choice was: (a) fix as drive-by commits on the same branch (recommended in the smoke-gate AskUserQuestion), (b) fix on a separate `fix/` branch off main, merge first, then rebase.

Option (a) won because: (1) my per-view 3-submit changes *uncovered* the BoneMatrix race (the 3·N submits/frame overhead increased the pending-queue depth enough to expose it), so it's part of validating the effort end-to-end; (2) the smoke gate explicitly requires "no validation warnings above INFO"; (3) the fixes are surgical (~10 LOC each) and don't entangle with the queue-routing code; (4) the rebase cost of option (b) is real (10 commits already on the branch). Both fixes documented in commit messages with VUID + the pre-existence rationale; future readers grepping for "VUID 03047" land on the explanation.

### Per-frame ring caches for AcquireImage wait, not monotonic timeline math

Pre-effort `AcquireImage` waited `m_FrameTimeline.Wait(frameIndex - MAX_FRAMES_IN_FLIGHT + 1)` — formulaic because the original `SubmitFrame` signaled exactly `frameIndex + 1`. With per-view 3-submit signaling monotonically per submit (multiple values per frame), that formula doesn't compose.

Two options:
- **Pre-compute the per-frame value:** track the highest signaled value per frame, infer the next frame's wait formulaically.
- **Cache it explicitly:** `m_LastGraphicsValuePerFrame[MAX_FRAMES_IN_FLIGHT]` ring, set when `SubmitView`'s `isLastView` fires.

Cache won. Reasons: (1) per-view 3-submit makes the formula `2 * (frameIndex - MAX_FRAMES_IN_FLIGHT + 1)` for the gA+gB pair, but only if every frame has the same view count — multi-view varies (editor toggles Scene/Game panels, frame-debugger opens/closes); (2) the cache is the same cost (one u64 write per frame's last submit) and self-documents the invariant "frame F is done when m_LastGraphicsValuePerFrame[F%N] retires"; (3) the compute side uses `0` sentinel for "no compute submit this frame" — formulaic math would have to special-case that branch anyway. `m_CurrentFrameLastComputeValue` accumulates across views within a frame so the ring cache captures the final value when only some views had compute work.

### GTAO chain routed, Cull stays graphics

AMD's gpuopen async-queues guidance explicitly warns about per-dispatch overhead: cross-queue semaphores cost roughly the same regardless of dispatch size, so tiny compute dispatches don't recoup the overhead. GPU frustum cull is one indirect-count fill per object (microseconds); routing it to async would add a cross-queue semaphore wait that takes longer than the dispatch itself. Same for the 4-cascade shadow cull. GTAO's 3-stage chain on the other hand is ~0.5–1 ms on a mid-range GPU at 1080p, with clean dependency boundaries (DepthPrepass output → GTAODepthPrefilter input, GTAODenoise output → Geometry PBR fragment input) — the cross-queue overhead is a small fraction of the chain's runtime, and the overlap with Shadow rendering is real.

### UploadImageMipped stays on graphics queue (BLIT requirement)

`vkCmdBlitImage` requires `VK_QUEUE_GRAPHICS_BIT` per Vulkan spec (VUID-vkCmdBlitImage-commandBuffer-cmdpool). The mipped-upload path therefore can't share the transfer ring with plain copies. Three options were on the table:
- **Pre-generate mips offline** (asset-pipeline mip cooker). Out of scope for this effort.
- **Compute-shader mip generation** (avoids the blit). Out of scope; would require a mip-gen shader + descriptor pipeline.
- **Parallel graphics-blit ring inside UploadContext.** Adopted.

The graphics-blit ring is structurally identical to the transfer ring — same 4-cmd round-robin, same `m_UploadTimeline` for completion tracking — just allocated from the graphics family. `UploadContext::CreateResources` creates both pools; `UploadImageMipped` calls `BeginBlitRingSlot` + `SubmitGraphics2` instead of `BeginTransferRingSlot` + `SubmitTransfer2`. `m_UploadTimeline` is shared because Vulkan timeline semaphores accept multi-queue signal (per spec) — `DrainPendingBinds` polls `GetValue()` agnostic of which queue retired. Future compute-shader-mip-gen effort can collapse both back into one ring.

---

## Plan-mode validation pass — what it caught

Three Explore agents ran in parallel against the codebase during plan-mode Phase 3. Five real gaps surfaced that became design changes rather than TODOs:

1. **Cross-queue barrier stage masks** — `GetStateInfo(ResourceState)` maps to graphics-only stages for `ShaderResource` / `ColorAttachment` / `DepthStencilAttachment`. Without explicit handling, the RG would have emitted graphics-stage barriers on the compute primary, immediately tripping VUID-vkCmdPipelineBarrier2-srcStageMask on first GTAO submit. Drove the `Barrier::crossQueueSrc` flag + TOP_OF_PIPE substitution.
2. **SceneDepth needs CONCURRENT** — the initial CONCURRENT-on-storage-only rule would have missed SceneDepth (no STORAGE_BIT, has SAMPLED_BIT). The cross-queue read by GTAODepthPrefilter would have hit `VK_QUEUE_FAMILY_IGNORED` validation. Drove the depth + sampled clause in the VKTexture opt-in policy.
3. **Multi-view shared-resource race** — single-primary-per-type-per-frame design would have raced on `m_ShadowMap` (view 2's shadow write overwriting view 1's PBR shadow read). Drove the per-view 3-submit topology and the per-view × per-frame cmd-buffer ring sizing (`MAX_VIEWS_PER_FRAME = 4`).
4. **GPUTimerPool single timestampPeriod** — Vulkan spec allows per-queue-family variation in `timestampValidBits`. Single device-level `m_TimestampPeriod` would silently corrupt compute-stream timer math if families diverged. Drove the startup-assertion that asserts cross-family compatibility, with per-family-period support deferred (rare hardware variation, would require per-queue query pools).
5. **FrameDebugger archive sink fragment-stage post-barrier** — the sink's `EmitArchiveCopy` post-copy barrier emits `dstStageMask = FRAGMENT_SHADER_BIT`, which is graphics-only. On compute primary it would trip the same VUID as #1. Drove the `IArchiveSink::OnPassExecuted` QueueFamily parameter + the `postDstStage` substitution.

Each gap was a thread that would have shipped if not surfaced — a useful proof that the plan-mode validation discipline (Phase 3 re-validation against arch docs + Explore-agent code audit) pays off on foundational efforts.

---

## Smoke fixes during the effort

Two issues surfaced mid-implementation and got fixed before the sub-task they touched landed:

**Non-primary LDR transition on wrong primary (5.5).** Surfaced when smoke-testing sub-task 5 (GTAO routed) with Scene + Game multi-view. The non-primary-view "transition LDR → SHADER_READ so scene view's ImGui can sample it" barrier was recording on `recorders.gA` because my sub-task 2 had aliased `primaryCmd = recorders.gA` for the local variable in `RenderPipeline::Execute`. gA runs before gB on the GPU timeline; the transition would precede PostProcess Composite's LDR write. Next frame's PostProcess pre-barrier expected LDR in COLOR_ATTACHMENT_OPTIMAL but found it in SHADER_READ_ONLY_OPTIMAL — VUID-vkCmdDraw-None-09600 at submit time. Fix: emit the barrier on `recorders.gB` so it runs as the last cmd in gB, after PostProcess + Outline finish writing LDR.

**Pre-existing GTAO storage-image UAB feature flag.** Surfaced when smoke-testing per-view 3-submit infra (sub-task 3a). GTAOMain's descriptor layout had `VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT` on its storage-image binding (binding 1, output AO image), but `VulkanContext::CreateLogicalDevice`'s required-features chain only enabled UAB for sampled-image / storage-buffer / uniform-buffer. NVIDIA was permissive; validation correctly flagged the spec violation. Pre-existing — fixed as a drive-by per the user's smoke-gate AskUserQuestion answer.

**Pre-existing BoneMatrix UAB.** Same smoke session as above. Per-frame slot cycling (game frame K writes slot K%3, render reads (K-1)%3) provides write/read isolation in steady state but breaks when GPU falls behind enough that frame K+3's game write hits a slot still referenced by frame K+1's pending cmd buffer. Per-view 3-submit's 3·N submits/frame increased the average pending-queue depth enough to expose the latent race — VUID 03047 fired three times (one per BoneMatrix slot). Fix: UAB on the binding + matching `UPDATE_AFTER_BIND_POOL` flags on layout + pool. Cycling still provides steady-state isolation; UAB is the safety net for pipeline-depth races.

---

## Build verification

Every commit builds clean Debug + Release on Windows MSVC. Pre-existing warnings only:
- `xutility` size_t→uint32_t conversion (stdlib template instantiation)
- `Model.cpp` size_t→uint32_t (3 sites)
- `shaderc_shared.lib` / `ws2_32.lib` / `dbghelp.lib` LNK4006 `__NULL_IMPORT_DESCRIPTOR` (Vulkan import descriptor)
- `Editor.cpp:616` chrono cast
- `ProjectLauncher.cpp` getenv deprecation
- `InspectorPanel.cpp:92` strncpy deprecation
- `Properties.cpp:182-184` sscanf deprecation

Smoke tests (user-driven):
- Boot editor; queue layout log fires correctly (`graphics=0, compute=2 (async), transfer=1 (async)` on RTX 3080).
- Scene + Game multi-view both render, visually parity with v2.11.1.
- No Vulkan validation warnings above INFO after the two drive-by fixes.
- GTAO toggle on/off works; compute submits stop when off (`m_LastComputeValuePerFrame[]` stays 0).
- Frame Debugger capture with GTAO routed — archives non-empty + uncorrupted.
- Window resize + minimize/restore loop — swapchain rebuild clean.
- 5+ minute steady-state run — no FPS drift, no `MemoryTracker` growth.

---

## Out of scope (deliberately)

- **EXCLUSIVE + QFOT barriers** for color RTs crossing queues. Documented as future-polish path in `arch/multi-queue.md`. Will become worth doing when forward-plus or gpu-particles introduces a compute pass writing a color RT (cluster light-list, particle render).
- **Multiple async-compute queues.** Discovery + cmd-pool array would extend trivially; pass routing would need a `QueueFamily::AsyncComputeN` enum or a queue-index parameter. No workload demands it today.
- **Present-from-compute.** DOOM-style latency optimization for present on the compute queue. Niche; orthogonal to the foundation.
- **Queue priority inversion** (Khronos sample pattern). Needs profiling data to tune; default priority for now.
- **Per-pass timeline signals.** Replace per-submit signals with per-pass for finer-grained sync (UE5 RDG "dependency level" pattern). Foundation supports it — just signal more often. No consumer demands it yet.
- **Transfer-queue route for `UploadImageMipped`** (compute-shader mip generation instead of `vkCmdBlitImage`). Would also handle BC compression as a side effect. Separate effort.
- **GPUTimerPool per-family timestampPeriod.** Asserts compatibility today; defers per-family-period math support to a hardware-driven need (rare on consumer GPUs).
- **Cull routed to async-compute.** Per AMD guidance, tiny dispatches don't recoup cross-queue overhead. Reconsider if a workload makes cull heavier.

---

## Files touched

**New (engine)**
- `luth/source/luth/renderer/QueueRecorders.h`

**New (docs)**
- `docs/development/arch/multi-queue.md`
- `docs/development/history/v2.x/async-compute-queue.md`

**Modified (engine)**
- `luth/source/luth/core/FrameData.h` (`MAX_VIEWS_PER_FRAME = 4`)
- `luth/source/luth/core/Version.h` (2.11.1 → 2.12.0)
- `luth/source/luth/memory/GPUTaggedPageAllocator.cpp` (CONCURRENT on backings + large-one-shot)
- `luth/source/luth/renderer/Renderer.{h,cpp}` (QueueRecorders API)
- `luth/source/luth/renderer/RenderBackend.h` (`SubmitView` virtual signature)
- `luth/source/luth/renderer/RenderPipeline.{h,cpp}` (`Execute(view, recorders)` + LDR transition on gB)
- `luth/source/luth/renderer/FrameDebugger.{h,cpp}` (queue-family hint + post-copy stage substitution)
- `luth/source/luth/renderer/FrameTargets.cpp` (covered by VKTexture sampled-depth opt-in)
- `luth/source/luth/renderer/ViewResources.cpp` (covered by VKTexture STORAGE_BIT opt-in)
- `luth/source/luth/renderer/backend/vulkan/VulkanBackend.{h,cpp}` (per-view × per-frame cmd rings + SubmitView + per-frame ring caches)
- `luth/source/luth/renderer/backend/vulkan/VulkanContext.{h,cpp}` (queue family discovery + per-queue submit wrappers + ApplyConcurrentSharing + timestamp assertion + storage-image UAB device feature)
- `luth/source/luth/renderer/backend/vulkan/VulkanTexture.cpp` (CONCURRENT iff STORAGE_BIT or sampled depth)
- `luth/source/luth/renderer/backend/vulkan/UploadContext.{h,cpp}` (dual ring: transfer + graphics-blit + sync2 + multi-queue signal)
- `luth/source/luth/renderer/rendergraph/IArchiveSink.h` (`OnPassExecuted` queue-family parameter)
- `luth/source/luth/renderer/rendergraph/RenderGraph.{h,cpp}` (QueueFamily enum + 4-arg AddComputePass + Execute(QueueRecorders) + Phase 2 dispatch + SolveBarriers cross-queue rule)
- `luth/source/luth/renderer/rendergraph/RenderGraphResources.h` (`QueueFamily` enum; `Barrier`/`BufferBarrier` `crossQueueSrc` flag)
- `luth/source/luth/renderer/rendergraph/RenderResourceCache.cpp` (CONCURRENT on transient images + buffers)
- `luth/source/luth/renderer/resources/BoneMatrixBuffer.cpp` (UAB on storage-buffer binding + pool/layout)
- `luth/source/luth/renderer/subsystems/GTAOSubsystem.cpp` (3 passes routed to AsyncCompute)
- `luth/source/luth/scene/systems/RenderingSystem.{h,cpp}` (per-view 3-submit loop; `InsertInterViewBarrier` removed)

**Modified (docs)**
- `CLAUDE.md` (Current Progress + Active series + Next)
- `docs/development/ROADMAP.md` (move from Planned → Completed)

---

## Memory entries — none new

No new workspace-memory entries from this effort. Existing entries continue to apply:
- `feedback_engine_lock_pattern.md` — std::mutex correct around `vkQueueSubmit2` (per-queue mutexes in this effort follow it)
- `feedback_smoke_gate_before_merge.md` — paused at sub-task 3a + 5 + final smoke-gate before this wrap-up
- `feedback_comment_style.md` — every new comment respects the ~120-char wrap + no widow lines + no banned patterns
