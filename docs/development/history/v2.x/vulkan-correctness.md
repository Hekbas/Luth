# v2.8.7 — vulkan-correctness

**Date:** 2026-04-28
**Commits:** 11 (on `refactor/vulkan-correctness`)
**Issue:** [#101](https://github.com/Hekbas/Luth/issues/101)

---

## Overview

Tier-1 Vulkan correctness pass before scaling to async-compute, forward+, and Jolt physics. Drove out from a deep architectural review of the renderer that turned up six concrete latent bugs / hardening gaps in the backend, RG, and frame submit path. All six landed (sub-tasks A–F); three rounds of audit afterwards added five more commits — the smoke test surfaced a 240 ms per-frame stall and a mutex-vs-SpinLock architectural mismatch (G + H), the architecture review found a timeline-monotonicity violation and a render-fiber blocking-call (I + J), and a comment-style audit collapsed verbose narrative blocks back to engine standard (K).

Internal foundation work; no user-visible behavior change. Tag-only release.

The work originally targeted v2.8.8 — animation-quick-pass (#93) was sequenced first as v2.8.7. Build order swapped because vulkan-correctness was the active branch when wrap-up came due. Animation-quick-pass slides to v2.8.8.

---

## Sub-tasks

| # | Sub-task | Commit |
|---|---|---|
| A | Thread-safe deletion queue (initial std::mutex version) | [`d1309f8`](../../../../commit/d1309f8) |
| B | RG: emit barrier on write-after-write even when state matches | [`e64b1a2`](../../../../commit/e64b1a2) |
| C | RG: `CullDeadPasses` transitive producer revival fix | [`741949c`](../../../../commit/741949c) |
| D | Swapchain robustness on resize (`AcquireImage`/`BeginFrame` → `bool`, OUT_OF_DATE handling, Init blocks on (0,0)) | [`6d41417`](../../../../commit/6d41417) |
| E | Device capability validation (Features2 + extension + surface support) | [`31495fa`](../../../../commit/31495fa) |
| F | `VkSubmitInfo2` for frame submit + RG-driven present transition (`postBarriers`, `finalState`) | [`e9d32d9`](../../../../commit/e9d32d9) |
| G | Smoke fix: present rebuild only on OUT_OF_DATE (240 ms stall on Windows DWM SUBOPTIMAL) | [`8342dfa`](../../../../commit/8342dfa) |
| H | Smoke fix: `SpinLock` for deletion queue (V1; std::mutex blocks the fiber's worker thread) | [`3f92807`](../../../../commit/3f92807) |
| I | Arch audit: drop spurious timeline host-signal on skip (would re-signal the same value at submit) | [`c785e09`](../../../../commit/c785e09) |
| J | Arch audit: defer Present rebuild to next Acquire (`vkDeviceWaitIdle` was running on the render fiber) | [`c62b4a5`](../../../../commit/c62b4a5) |
| K | Comment-style audit: collapse multi-line blocks to ≤120-col single lines, drop history phrasing | [`95f0288`](../../../../commit/95f0288) |
| –  | Wrap-up (this file, version bump, ROADMAP/BACKLOG/CLAUDE.md updates, merge + tag) | (this commit) |

---

## Investigation arcs

### G + H — smoke test

User reported "abysmal framerate" with ~240 ms gaps between Present and the next AcquireNextImage in Tracy, and flagged that the deletion queue's std::mutex was at odds with the engine's fiber-aware locking convention.

**G — Present rebuilding on SUBOPTIMAL.** Sub-task D's swapchain rebuild path triggered on both `VK_ERROR_OUT_OF_DATE_KHR` and `VK_SUBOPTIMAL_KHR`. Windows DWM raises SUBOPTIMAL constantly under fractional display scaling — the surface succeeds, but the extent reported by the compositor differs by 1 pixel from the swapchain's. With `Recreate` on every SUBOPTIMAL Present, `vkDeviceWaitIdle` ran every frame, fully draining the GPU. Acquire's path already (correctly) ignored SUBOPTIMAL; Present was reverted to the same policy.

**H — std::mutex on a fiber path.** The deletion queue's PushDeletion is called from any thread that releases the last `shared_ptr` to a `VKTexture` / `VKBuffer` — worker fibers included. `std::mutex` on Windows uses an SRWLock that blocks the *underlying OS thread* under contention, which means a fiber holding the mutex parks the entire worker thread (other ready fibers on that worker can't preempt). The engine's convention for hot-path locks is `Luth::SpinLock` from `luth/jobs/SpinLock.h` (V1: <100 cycles, never yield under lock) — already used by `CommandAllocatorPool`. Critical section in PushDeletion is a single push_back; in FlushDeletionQueue it's a swap, with deletor execution kept outside the lock — well within the contract.

### I + J — architectural audit

User pushed back on whether the rest of the branch had similar architecture mismatches I'd missed. Re-read [`fiber-system.md`](../../arch/fiber-system.md), [`frame-pipeline.md`](../../arch/frame-pipeline.md), and `FrameData.h`. Two more issues in sub-task D surfaced:

**I — spurious timeline host-signal on skip.** When AcquireImage failed, the original D code called `m_FrameTimeline.Signal(frameIndex + 1)` to "keep the wait chain monotonic". This was reasoning from the wrong premise. App.cpp's main loop `continue`s on skip *before* `m_FrameData.Advance()`, so the same frameIndex retries on the next iteration; when SubmitFrame eventually succeeds, it tries to signal frameIndex+1 *again*. Vulkan timeline values must strictly increase — re-signaling the same value is a validation error and undefined-behavior territory on some drivers. The retry-without-advance pattern keeps the wait chain consistent on its own; the host-signal was both unnecessary and incorrect.

**J — Recreate from Present runs on the render fiber.** `Renderer::EndPrimaryCmdAndSubmit` is called from `RenderingSystem::Update`, which runs as a worker-fiber stage in v2.8.4 pipeline-phase-3. So `VulkanSwapchain::Present`'s OUT_OF_DATE path — which calls `Recreate` → `vkDeviceWaitIdle` — was running on the render fiber. `vkDeviceWaitIdle` is a long blocking call; on a fiber it parks the underlying worker thread (fiber-system.md: zero OS-thread blocking on hot path). Acquire's rebuild is fine because Acquire is called from `Renderer::BeginFrame` on the main thread (V2-isolated, blocking is correct there). Fix flipped Present to set a `m_NeedsRebuild` flag and return immediately; the next AcquireNextImage consumes the flag and runs Recreate where it belongs.

### K — comment style

User flagged that the comments I'd added across A–J were too verbose, contained history language ("now", "previously", "we changed X to Y"), and were folded at ~80 cols instead of the ≤120-col engine standard. K swept all the comment additions on the branch, collapsing multi-line blocks to single ~120-col lines, dropping history phrasing, and replacing paraphrased explanations of fiber-system.md with short "V1"/"V2" tokens that match the convention already used in the codebase. Net −74 lines across 12 files; no behavior change.

---

## Architectural changes

- **`VulkanContext::m_DeletionLock`** — replaces the initial std::mutex with `Luth::SpinLock`. Resource dtors push from any thread; FlushDeletionQueue drains under lock then runs deletors outside (a deletor may push when releasing nested resources). `m_QueueMutex` and `m_CommandPoolMutex` keep std::mutex — those wrap kernel syscalls (vkQueueSubmit, vkAllocateCommandBuffers) and exceed the SpinLock contract.

- **`RG::ResourceNode::lastWriter` + `BufferNode::lastWriter`** — pass index of the most recent writer per resource. Drives WAW barrier emission in `SolveBarriers`: two consecutive same-state writes still need a Vulkan execution barrier between them, which the previous "emit on state change only" path missed. Currently masked because no pass writes the same resource twice in a row, but landed before the next pass that would (e.g., a future bloom downsample chain) lights it up.

- **`RG::PassNode::postBarriers` + `ResourceNode::finalState`** — image transitions emitted *after* the pass body has executed and the archive sink has had a chance to copy attachments. Used so that external resources can declare a final state at import time and have the RG drive the closing barrier — eliminates the hardcoded `oldLayout = COLOR_ATTACHMENT_OPTIMAL` present-barrier in `Renderer::EndPrimaryCmdAndSubmit`. ImGuiPass imports the backbuffer with `finalState = Present`; SolveBarriers walks external resources after the main loop and appends the postBarrier to whichever pass last wrote them.

- **`RG::ImportResource(desc, image, view, initialState, finalState)`** — third overload, distinct from the existing `(initialState, baseArrayLayer, layerCount)` shape by parameter type. Backbuffer import in ImGuiPass moves to the new overload.

- **`VulkanBackend::AcquireImage` returns `bool`** — false signals frame-skip. Callers (`Renderer::BeginFrame`, then `App::Run`) propagate; App's main loop yields and continues without advancing `m_FrameData`. Same frameIndex retries on the next iteration. `RenderBackend::AcquireImage` virtual signature changed alongside (no other implementers exist; only Vulkan today).

- **`VulkanSwapchain::m_NeedsRebuild`** — set by Present on OUT_OF_DATE, consumed at the top of the next AcquireNextImage. Keeps `vkDeviceWaitIdle` off the render fiber.

- **`VulkanSwapchain::Init` blocks on (0,0) extent** — launched-minimized / OS-not-yet-committed window state would otherwise fail swapchain creation. Loop yields via `glfwWaitEvents` until the window has a paintable size.

- **`VulkanContext::PickPhysicalDevice` + `CreateLogicalDevice`** — capability-validate the chosen device. Picker gates on `VK_KHR_swapchain` + a graphics queue family before considering type (prefer discrete, fall back to first eligible). `CreateLogicalDevice` queries `vkGetPhysicalDeviceFeatures2` and aborts with a clear log listing which 1.1/1.2/1.3 features are missing if any required one (`shaderDrawParameters`, `descriptorBindingPartiallyBound`, `descriptorBindingSampledImageUpdateAfterBind`, `runtimeDescriptorArray`, `shaderSampledImageArrayNonUniformIndexing`, `timelineSemaphore`, `dynamicRendering`, `synchronization2`) reports unsupported. Surface-presentation support is asserted post-`CreateSurface` in `VulkanSwapchain` (the surface doesn't exist yet at PickPhysicalDevice time).

- **`VulkanContext::Submit2`** — sync2 sibling to `Submit`. `VulkanBackend::SubmitFrame` rewritten to use `VkSubmitInfo2` + `VkSemaphoreSubmitInfo` + `VkCommandBufferSubmitInfo`, collapsing the legacy timeline-semaphore extension struct. The frame's wait/signal stages move from `VK_PIPELINE_STAGE_*` to `VK_PIPELINE_STAGE_2_*`. Matches the rest of the stack — RG barriers were already sync2.

- **`Renderer::EndPrimaryCmdAndSubmit` is now ~3 lines** — vkEndCommandBuffer + Submit. The hardcoded present barrier (legacy `vkCmdPipelineBarrier`, hardcoded `oldLayout = COLOR_ATTACHMENT_OPTIMAL`) is gone — the RG owns it.

---

## Build verification

All commits build clean Debug + Release on Windows MSVC. No new warnings; pre-existing only (`Editor.cpp` chrono cast, vulkan-1.lib `LNK4006` import-descriptor warnings, `ProjectLauncher.cpp` getenv).

Smoke tests (user-driven):

- Launch + 3+ viewport resizes + minimize/restore + 30 s scene navigation + close — no validation messages above WARN.
- Tracy: no `vkDeviceWaitIdle` spikes on the render-stage fiber (J fix).
- Tracy: tight present timing against vsync; no per-frame Recreate stall (G fix).
- Window resize loop (rapid drag): swapchain rebuild executes on main thread (Acquire path), not the render fiber (J).
- Frame Debugger capture across all four play states: archives still render correctly post-RG-driven-present.

---

## Out of scope (deliberately)

- Persistent CPU-mapped buffers (`m_ObjectSSBO`, `m_IndirectBuffer`, material SSBO) are still single-buffered. The frame-fence in AcquireImage waits for `frame - MAX_FRAMES_IN_FLIGHT + 1`, leaving frames N-1 and N-2 potentially in-flight while frame N writes — the buffers can be overwritten while the GPU is still reading them. Carried into `persistent-buffer-ring` (v2.8.9).
- VMA `CPU_TO_GPU` is deprecated and the engine has no `vkFlushMappedMemoryRanges` calls — works in practice on PC GPUs because they expose host-coherent memory, but not spec-guaranteed. Same v2.8.9 epic.
- Shader hot-reload still calls `vkDeviceWaitIdle` per save and runs at the top of every view's Execute (twice per frame when both Scene + Game viewports are open). `shader-reload-async` (v2.8.10) replaces this with a deferred-destroy of old VkPipeline / VkShaderModule via the (now SpinLock-safe) deletion queue.
- `RenderResourceCache` linear lookup, 10 000-frame stale threshold, and missing usage-flag in the pool key. `vulkan-polish` (v2.8.11).
- `ImmediateSubmit` synchronously stalls the calling thread for asset uploads; should route through the existing `UploadContext` and a transfer queue. `vulkan-polish` (v2.8.11).
- Async compute queue. Compute passes (cull, GTAO) currently serialize on the graphics queue — independent of vulkan-correctness's scope. `async-compute-queue` (v2.9.2), prereq for `forward-plus`.
- Render-graph aliasing. Lifetime tracking exists in the RG (B added `lastWriter`; `firstPass`/`lastPass` were already there) but isn't consumed for memory aliasing yet. `rg-aliasing` (v2.9.3, optional).

---

## Memory entries shipped

Three entries added to the workspace memory during the effort:

- `feedback_engine_lock_pattern.md` — hot-path locking uses `Luth::SpinLock`, never `std::mutex` (recorded after H).
- `reference_arch_docs.md` — read `docs/development/arch/*.md` at session start; CLAUDE.md alone is insufficient (recorded after I + J).
- `feedback_comment_style.md` — comments cap at 120 col, why-only, no history phrasing, one line beats a block (recorded after K).
