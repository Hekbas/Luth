# Frame Pipeline — Triple-Buffered Execution

## Pipelined Frame Model

```text
 Time ──────────────────────────────────────────────►

 Frame N:     [── Game Logic (CPU) ──────────────]
 Frame N-1:                [── Render Record (CPU) ──────]
 Frame N-2:                              [── GPU Execute ──]
                                                   ▲
                                              PollerJob wakes
                                              fibers, frees
                                              Tag(N-2) memory
```

- `MAX_FRAMES_IN_FLIGHT = 3` (unified across all files)
- `FrameData` owns per-frame `FrameContext` with `SpinLock` (no `std::mutex`)
- GPU completion polled by `VulkanWaitJob` (Timeline Semaphore), never `vkWaitForFences`
- When GPU N-2 completes, overflow allocator pages (V6) are reclaimed

## RenderGraph Execution Model

```
For each non-culled pass (in topological order):
  1. Batch all pre-barriers → single vkCmdPipelineBarrier2
  2. Dispatch RenderPassJob (records secondary cmd buffer on worker fiber)
  3. WaitForCounter (V5 inline execution if depth allows)
  4. BeginRendering → ExecuteCommands → EndRendering on primary
```

Inter-pass ordering is serial (barrier correctness). Parallelism is within a pass (command recording on worker fibers).
