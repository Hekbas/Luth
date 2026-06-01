# Multi-Queue Vulkan — Foundation

## Topology

```text
GPU engines (discrete hardware):
  Graphics engine  ─── raster + compute
  Compute engine   ─── async-compute (compute-only family)
  Copy/DMA engine  ─── transfer-only family

Queue handles (VulkanContext):
  m_GraphicsQueue   ── always present
  m_ComputeQueue    ── distinct family if found; else aliased to graphics
  m_TransferQueue   ── distinct family if found; else aliased to graphics
```

Single-family GPUs (Intel iGPU, some mobile) collapse all three queues to one `VkQueue`; the `SubmitGraphics2 / SubmitCompute2 / SubmitTransfer2` wrappers dispatch invariantly so call sites stay clean.

## Queue family discovery

Priority order in `VulkanContext::CreateLogicalDevice`:

```
graphicsFamily = first family with GRAPHICS_BIT (baseline; asserted)

computeFamily  = first with COMPUTE_BIT  AND NOT GRAPHICS_BIT
                 fallback: graphicsFamily

transferFamily = first with TRANSFER_BIT AND NOT GRAPHICS_BIT AND NOT COMPUTE_BIT  (true DMA engine)
                 then    : first with TRANSFER_BIT AND NOT GRAPHICS_BIT
                 fallback: graphicsFamily
```

Discovered layout logs at `LH_CORE_INFO("Queue layout — graphics={N}, compute={M} (async|aliased), transfer={K} (async|aliased)")`. `m_ComputeIsAsync` / `m_TransferIsAsync` flags drive `VkDeviceQueueCreateInfo` dedup — one entry per distinct family.

## Three timelines

| Semaphore | Owner | Signaled by | Increment |
|---|---|---|---|
| `m_FrameTimeline`   | `VulkanBackend` | every graphics submit (gA + gB per view)              | per submit |
| `m_ComputeTimeline` | `VulkanBackend` | every async-compute submit (per view with compute work) | per submit |
| `m_UploadTimeline`  | `UploadContext` | every transfer or graphics-blit submit                | per upload |

Per-submit monotonic — values rise as work is queued; the final value per frame is cached in `m_LastGraphicsValuePerFrame[]` / `m_LastComputeValuePerFrame[]` (sized `MAX_FRAMES_IN_FLIGHT`) so `AcquireImage` knows exactly which value gates GPU-N-2 retirement. `0` sentinel in the compute ring slot = no compute work that frame.

## Per-view 3-submit topology

```text
Frame F, N views queued (typical N = 1 or 2):

  view K ∈ [0..N):
    gA submit  (graphics queue):
      wait    = K == 0 ? prevFrame m_ComputeTimeline @ lastComputeValue @ ALL_COMMANDS (if any)
                         (cross-frame WAR: gA's ShadowPass write of a persistent resource — the single
                          shadow map — must not race the previous frame's async-compute still reading it)
                       : m_FrameTimeline @ prevViewGBSignal @ EARLY_FRAGMENT_TESTS
                         (replaces the legacy inline InsertInterViewBarrier — same stage relationship,
                          synchronises view K's fragment-shader reads → view K+1's depth-write)
      signal  = m_FrameTimeline @ (next monotonic value)

    compute submit  (compute queue, only if view's RG routed any pass to AsyncCompute):
      wait    = m_FrameTimeline @ thisViewGASignal @ ALL_COMMANDS
                  (ALL_COMMANDS, not COMPUTE_SHADER — gates the reader's cross-queue layout transition,
                   whose barrier src is the empty TOP_OF_PIPE scope; see the cross-queue rule below)
      signal  = m_ComputeTimeline @ (next monotonic value)

    gB submit  (graphics queue):
      wait    = hasCompute ? m_ComputeTimeline @ thisViewComputeSignal @ ALL_COMMANDS
                           : m_FrameTimeline   @ thisViewGASignal       @ ALL_GRAPHICS
              + (K == N-1) imageAvailable @ ALL_COMMANDS (acquire gates the swapchain's first transition)
      signal  = m_FrameTimeline @ (next monotonic value)
              + (K == N-1) renderFinished[imageIdx]

  After last view's gB:
    m_LastGraphicsValuePerFrame[F % N] = lastGBSignal
    m_LastComputeValuePerFrame [F % N] = lastComputeSignalOrZero   // 0 sentinel
    vkQueuePresentKHR waits on renderFinished[imageIdx].
```

Empty gA / gB / compute primaries are valid no-op submits. Per-view 3-submit means 3·N submits per frame (typical N = 2 → 6 submits, ~60–300 µs/frame Windows submit overhead, dwarfed by intra-frame compute-overlap gains).

`AcquireImage` predicate (replaces the prior `frameIndex - MAX_FRAMES_IN_FLIGHT + 1` formula):

```cpp
const u32 retiringSlot = (frameIndex - MAX_FRAMES_IN_FLIGHT) % MAX_FRAMES_IN_FLIGHT;
m_FrameTimeline.Wait(m_LastGraphicsValuePerFrame[retiringSlot]);
if (m_LastComputeValuePerFrame[retiringSlot] > 0)
    m_ComputeTimeline.Wait(m_LastComputeValuePerFrame[retiringSlot]);

// V6 reclaim — page tag T is referenced by iter T (game-stage) and iter T+1 (render-stage).
// Both retire when graphics signals frame T+2's last value; safe-to-free tag = (frameJustRetired - 2).
const u64 frameJustRetired = frameIndex - MAX_FRAMES_IN_FLIGHT + 1;
if (frameJustRetired >= 2) {
    Memory::TaggedPageAllocator   ::Get().FreeTag((u32)(frameJustRetired - 2));
    Memory::GPUTaggedPageAllocator::Get().FreeTag((u32)(frameJustRetired - 2));
}
```

## RenderGraph queue routing

`PassNode::queueFamily ∈ { Graphics, AsyncCompute }`. Default Graphics — opt into async via the 4-arg `AddComputePass<Data>(name, QueueFamily::AsyncCompute, setup, execute)`. `RG::Execute(QueueRecorders, timers) → bool hasComputeWork`. Phase 2 dispatches per pass:

```
seenAsyncCompute = false
for each non-culled pass:
    primary = AsyncCompute   ? recorders.compute
            : seenAsyncCompute ? recorders.gB
            :                    recorders.gA
    if (AsyncCompute) { seenAsyncCompute = true; hasComputeWork = true; }
    emit pre-barriers → execute body → emit post-barriers (all on `primary`)
```

## Cross-queue barrier rule (TOP_OF_PIPE substitution)

`SolveBarriers` tags each emitted `Barrier::crossQueueSrc` when `m_Passes[lastWriter].queueFamily != currentPass.queueFamily`. At emission, the barrier's src side is forced to `VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT` / `VK_ACCESS_2_NONE`:

```cpp
if (b.crossQueueSrc) {
    srcStage  = VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT;
    srcAccess = VK_ACCESS_2_NONE;
}
// dstStage / dstAccess / layout transition unchanged.
```

Rationale: a graphics-only src stage emitted on the compute primary violates VUID-vkCmdPipelineBarrier2-srcStageMask, so the src is dropped to the empty `TOP_OF_PIPE` / `NONE` scope and the cross-queue *semaphore* carries the dependency.

**Wait-stage pairing (corrected after the `vulkan-sync-hardening` sweep — this was a real bug).** An empty source scope forms an execution-dependency chain with the carrying semaphore *only* when the wait's dst stage covers it — i.e. the cross-queue wait must be `ALL_COMMANDS` (logically later than every stage). A narrower wait (`COMPUTE_SHADER` / `FRAGMENT_SHADER`) leaves the reader's layout-transition *write* ungated and races the cross-queue producer (sync-val `WRITE_AFTER_WRITE`). The per-view cross-queue waits above are therefore `ALL_COMMANDS`; cross-*frame* reuse of a persistent resource on two queues (shadow map written on graphics, read on async-compute) is closed by the first-view gA → prev-frame-compute wait. Ref: Khronos cross-queue layout-transition example (its `srcStage=NONE` assumes `pWaitDstStageMask=TOP_OF_PIPE`). N-buffering those resources would let the cross-frame wait relax — deferred; `lastReader`/first-consumer sync placement (UE5 RDG) needs per-pass semaphores, also deferred.

Same logic for buffer barriers and external `finalState` post-barriers when the consumer is on a different queue (today only swapchain Present, which stays on graphics — no cross-queue case exercised yet).

## Resource sharing — CONCURRENT opt-in policy

`VulkanContext::ApplyConcurrentSharing(VkBufferCreateInfo&)` / `(VkImageCreateInfo&)` set `sharingMode = CONCURRENT` over the deduped `{graphics, compute, transfer}` family list. Size-1 deduped list (single-family GPU) silently falls back to EXCLUSIVE — CONCURRENT with one family is implementation-defined.

| Resource | Site | Policy |
|---|---|---|
| `GPUTaggedPageAllocator` 64 MB backings + large-one-shot | `GrowBackingPoolLocked` / `AllocateLargeTagged` | CONCURRENT — universal CPU→GPU data path; future async-compute SSBO consumers (forward-plus, gpu-particles) cross queues |
| RG transient images + buffers | `RenderResourceCache::GetTexture` / `GetBuffer` | CONCURRENT |
| Persistent `VKTexture` | `VKTexture::CreateImage` | CONCURRENT iff `STORAGE_BIT` (compute outputs) **OR** (`DEPTH_STENCIL_ATTACHMENT_BIT` + `SAMPLED_BIT`) (depth sampled by compute — SceneDepth → GTAODepthPrefilter); else EXCLUSIVE |
| `UploadContext` staging | `CreateResources` | EXCLUSIVE — transfer-queue-only |
| Color RTs (RGBA16F SceneColor, RGBA8 LDR) | per-format VKTexture path | EXCLUSIVE — preserves AMD Delta Color Compression |
| Geometry vertex/index buffers | `VulkanBuffer` | EXCLUSIVE — graphics-queue-only |
| Swapchain images | `VulkanSwapchain` | EXCLUSIVE — graphics presents |

**Why CONCURRENT, not EXCLUSIVE + QFOT.** NVIDIA driver guidance: `VkSharingMode` is ignored — CONCURRENT carries zero overhead. AMD's only documented concern is DCC on color render targets, which the per-resource opt-in preserves. EXCLUSIVE + QFOT (paired release/acquire `VkImageMemoryBarrier2` per cross-queue handoff) is a documented future-polish path — would require `SolveBarriers` to emit paired barriers on both queues' primaries. Not done because: (1) zero in-scope perf delta on the current hardware target, (2) the barrier solver complication isn't justified until forward-plus / gpu-particles surface a workload where DCC on a cross-queue color RT actually matters.

## UploadContext — dual ring (transfer + graphics-blit)

`UploadBuffer` + `UploadImage` (plain `vkCmdCopyBuffer` / `vkCmdCopyBufferToImage`) submit on the transfer queue via the transfer ring. `UploadImageMipped` submits on the graphics queue via a parallel graphics-blit ring — `vkCmdBlitImage` requires `VK_QUEUE_GRAPHICS_BIT` per spec (VUID-vkCmdBlitImage-commandBuffer-cmdpool).

Both rings signal the **same** `m_UploadTimeline` — Vulkan timeline semaphores accept multi-queue signal; `DrainPendingBinds` polls `GetValue()` agnostic of which queue retired the fence.

```text
UploadBuffer / UploadImage     ─► transfer family ─► SubmitTransfer2 ─► m_UploadTimeline ↑
UploadImageMipped              ─► graphics family ─► SubmitGraphics2 ─► m_UploadTimeline ↑
                                                                       │
                                                                       ↓
                                                      DrainPendingBinds polls GetValue() →
                                                      BindlessDescriptorSet::BindTexture
```

Stage masks on the transfer queue: only `TRANSFER_BIT`, `TOP_OF_PIPE_BIT`, `BOTTOM_OF_PIPE_BIT`, `ALL_COMMANDS_BIT` valid. `UploadImage`'s post-barrier uses `dstStage = BOTTOM_OF_PIPE_BIT` (the upload-fence wait chain supplies the dependency to the graphics-side fragment-shader read).

## Acceleration structure builds — compute-family operation

`vkCmdBuildAccelerationStructuresKHR` requires `VK_QUEUE_COMPUTE_BIT` on the command pool per the Vulkan 1.3 spec — eligible on the graphics queue family (which always advertises compute), the dedicated compute family, and any other family that exposes compute. Specifically NOT required: graphics. This contradicts pre-`rt-renderer` arch text that called for the graphics queue. NVIDIA's "Best Practices for NVIDIA RTX Ray Tracing" explicitly recommends async-compute for AS building so it overlaps with rasterized G-buffer / shadow work.

Per `rt-renderer.B.2`, the per-frame `TlasBuildPass` (which orchestrates skinning compute dispatch + skinned-BLAS refit + TLAS rebuild) routes to `QueueFamily::AsyncCompute` via the 4-arg `RenderGraph::AddComputePass` overload. The cross-queue handoff to a future B.3 RT consumer is covered by the per-submit timeline semaphore wait at the next graphics submit + the existing `TOP_OF_PIPE` substitution for cross-queue barriers. The synchronous import-time BLAS build path (`VKAccelerationStructure::CreateStaticBLAS` / `CreateSkinnedBLAS`) uses `VulkanContext::ImmediateSubmit`, which runs on the graphics queue — still spec-correct because graphics families advertise compute. Tagged-heap backings already opt into `CONCURRENT` sharing per the policy below, so cross-queue scratch consumption works without QFOT.

## Bindless registration

Unchanged from the single-queue path. `UploadContext::PushPendingBind(outIndex, view, sampler, fenceValue)` records the pending registration with the upload fence value. `DrainPendingBinds` (called from `AssetManager::Update`) polls `m_UploadTimeline.GetValue()` and writes the bindless descriptor (CPU-side `vkUpdateDescriptorSets`, queue-agnostic) once the fence retires. The transfer queue's submit signals `m_UploadTimeline` — same as the graphics queue would — so the pump is invariant.

## FrameDebugger archive sink — queue-aware

`IArchiveSink::OnPassExecuted(passIndex, RG&, VkCommandBuffer, QueueFamily)` — queue family added for stage-mask compatibility. `vkCmdCopyImage` itself is queue-agnostic (`VK_QUEUE_TRANSFER_BIT` is implicit on graphics + compute families per spec). The post-copy "make available to next consumer" barrier substitutes `dstStageMask = COMPUTE_SHADER_BIT` when on the compute primary (instead of the graphics-default `FRAGMENT_SHADER_BIT`). The cross-queue semaphore at the next submit boundary supplies graphics-visible memory ordering.

## GPUTimerPool — single-period assumption

`GPUTimerPool` uses one shared `VkQueryPool` per frame and one device-level `timestampPeriod`. `vkCmdWriteTimestamp2` accepts cmd buffers from any queue family per spec — timestamps from different queues land in the same pool. Single period is correct as long as in-use queue families share `timestampValidBits`.

`VulkanContext::CreateLogicalDevice` asserts compatibility post-discovery (compute + transfer `timestampValidBits` must match graphics where non-zero). Rare hardware divergence aborts with `LH_CORE_CRITICAL` rather than silently corrupting timer math. Per-family-period support is deferred — would require either per-queue query pools or per-timestamp queue metadata.

## V1–V6 hazard composition

| Hazard | Multi-queue composition |
|---|---|
| **V1** SpinLock <100 cycles, no yield under lock | Per-queue mutexes (`m_QueueMutex` + siblings) wrap `vkQueueSubmit2` — kernel syscalls, std::mutex remains correct (documented exception per `arch/memory.md`). Hot-path allocator code stays on `SpinLock`. |
| **V2** Main thread isolated | Render stage runs on worker fiber; SubmitView called from render stage. Main thread never enters submit path. |
| **V3** `VkCommandBuffer` thread affinity | Per-queue `CommandAllocatorPool` instances (graphics + compute). Each pool is queue-family-scoped via its ctor parameter; `RecordingScope` invariant applies per pool. |
| **V4** `WaitOnAddress` lost wakeup | No new condvar waits. Timeline polling is GPU-driven via `VulkanWaitJob`. |
| **V5** Sub-job context switch | RG `Execute` single yield (Phase 1 parallel record + WaitForCounter); per-view 3-submit happens serially in main loop, no new fiber switches. |
| **V6** GPU stall ↔ allocator deadlock | `FreeTag(N-2)` predicate extends to wait on **both** per-frame ring caches before reclaim. Overflow tier unchanged; frame N still gets pages even when GPU stalls on N-2. |

## Validation expectations

- No `VUID-vkCmdPipelineBarrier2-srcStageMask-*` for cross-queue barriers — TOP_OF_PIPE substitution **plus** the `ALL_COMMANDS` cross-queue waits cover it (the substitution alone does *not* — see the cross-queue rule above).
- No cross-queue `SYNC-HAZARD-WRITE_AFTER_WRITE` — the `ALL_COMMANDS` cross-queue waits gate the reader's empty-source-scope layout transition; the first-view gA → prev-frame-compute wait closes the cross-frame persistent-resource (shadow map) case.
- No `VUID-vkCmdBlitImage-commandBuffer-cmdpool` — `UploadImageMipped` stays on graphics queue.
- No `VK_QUEUE_FAMILY_IGNORED` complaints on cross-queue resources — CONCURRENT applied per the opt-in policy.
- No `VUID-vkCmdDraw-None-09600` from descriptor layout-mismatch — per-frame descriptor cycling preserved (game frame K writes slot K%3; render reads (K-1)%3 — distinct), and UAB on BoneMatrix + GTAOMain handles the pipeline-depth race that per-view 3-submit overhead exposes.

## Single-family fallback

On a GPU with only one queue family (Intel iGPU, some mobile), the priority-order discovery resolves all three families to graphics. The submit wrappers all dispatch through `VulkanContext::Submit{Graphics,Compute,Transfer}2` to the same `VkQueue` handle. CONCURRENT opt-in via `ApplyConcurrentSharing` silently falls back to EXCLUSIVE (size-1 deduped list). RG queue routing still works — `AsyncCompute` passes route to `recorders.compute`, which is the same cmd buffer-allocation-family as graphics but recorded into a distinct primary. The 3-submit topology still fires (`gA → compute → gB` per view), with all three submits hitting the same queue serially. Per-view ordering is preserved through the same timeline-semaphore chain; performance regresses to "single-queue but with more submit overhead". This is the spec-correct degenerate path — explicit testing recommended on iGPU.

## Smoke-test checklist

1. Boot editor; startup log prints `Queue layout — graphics=N, compute=M (async|aliased), transfer=K (async|aliased)`.
2. Load scene; visual parity with single-queue baseline.
3. Multi-view (Scene + Game panels): per-view 3-submit visible in RenderDoc (`3 × num_views` submits/frame); no validation above INFO.
4. Tracy GPU view: GTAO compute zones on compute stream, ideally overlapping shadow render on graphics.
5. Toggle GTAO off (PostProcess settings): `m_LastComputeValuePerFrame[]` stays 0; no compute submits issued.
6. Run 5+ min; steady FPS; no validation errors; no `MemoryTracker` growth.
7. Frame Debugger capture with GTAO routed; click into GTAO passes; archives non-empty + not corrupted.
8. Window resize + minimize/restore loop; swapchain rebuild clean.
9. If iGPU accessible: graceful degenerate-path behavior (one queue, serial submits).

## Future extension points

- **Multiple async-compute queues.** Discovery + cmd-pool array would extend trivially; pass routing would need a `QueueFamily::AsyncCompute2 / N` enum or a queue-index parameter.
- **Present-from-compute.** DOOM-style latency optimization for present on the compute queue. Niche; orthogonal to the current foundation.
- **Queue priority inversion.** Khronos sample pattern — late-frame work high-priority, early-next-frame low-priority. Single `VkDeviceQueueCreateInfo::pQueuePriorities` edit; needs profiling data to tune.
- **EXCLUSIVE + QFOT** for color RTs crossing queues. `SolveBarriers` would emit paired release/acquire barriers on the two queues' primaries; would let AMD DCC stay enabled on cross-queue color RTs (when forward-plus or particles introduce one).
- **Per-pass timeline signals.** Replace per-submit signals with per-pass for finer-grained sync (UE5 RDG "dependency level" pattern). Foundation supports it — just signal more often.
- **Transfer-queue route for `UploadImageMipped`.** Mip generation via compute shader instead of `vkCmdBlitImage` would move the whole mipped path off the graphics queue; would also handle BC compression as a side effect.
