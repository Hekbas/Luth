# Frame Pipeline — Triple-Buffered Execution

## Pipelined Frame Model

```text
 Time ──────────────────────────────────────────────►

 Frame N:     [── Game Logic (CPU) ──────────────]
 Frame N-1:                [── Render Record (CPU) ──────]
 Frame N-2:                              [── GPU Execute ──]
                                                   ▲
                                              GPU completion
                                              releases overflow
                                              pages + deletion
                                              queue for slot
```

- `MAX_FRAMES_IN_FLIGHT = 3`
- `FrameData` owns per-frame `FrameContext` with `SpinLock` (no `std::mutex`)
- GPU completion polled by Timeline Semaphore (`vkWaitForFences` is unused)
- When GPU N-2 completes, overflow allocator pages (V6) are reclaimed

## Stage Dispatch — concurrent CPU pipelining (v2.8.4)

`App::Run` dispatches the two CPU stages onto worker fibers each iteration
and busy-spins on their counters from the main thread (V2 isolated).

**Frames 0/1 (sync warm-up).** Pipeline is cold; render targets `Current()`
synchronously after the GameReady wait. Seeds the timeline + descriptor
state so frame 2 can transition to steady.

**Frame ≥2 (steady).** Render-stage of `Previous()` is dispatched
*before* the GameReady wait. The two stages overlap on separate worker
fibers; main thread waits on both counters before advancing.

```
main: Execute(GameStageFn,   &Current.GameReady)
main: Execute(RenderStageFn, &Previous.RenderReady)   // steady only
main: WaitForCounter(&Current.GameReady)
main: WaitForCounter(&Previous.RenderReady)
main: Advance
```

The frame boundary between stages is `RenderSnapshot` (a POD on
`FrameContext`, spans into `LogicMemory`), captured at end of game stage
once the ECS is coherent. The render stage walks the snapshot, never the
registry — no shared mutable state to lock.

## Stage isolation

`MaterialSystem` and `BoneMatrixBuffer` retain their `std::mutex`
(D6 — converting to lock-free was out of scope). Discipline is enforced
by `JobSystem::Stage` propagated from the dispatching parent fiber to
sub-jobs via `Job::StageTag`. Mutator entry points
`assert(GetCurrentStage() == Stage::Game)`.

## RenderGraph Execution Model

Two-phase dispatch, single yield per frame.

**Phase 1.** Each non-culled graphics pass dispatches a `RenderPassJob`
onto worker fibers against a single counter, recording into its own
secondary cmd buffer. One `WaitForCounter` collects them all.

**Phase 2.** Walk the passes in order on the primary cmd buffer:
batched pre-barriers, then either compute exec inline or
`BeginRendering` + `vkCmdExecuteCommands(secondary)` + `EndRendering`.
Pass-order semantics and barrier correctness are preserved exactly.

```
Phase 1 (parallel):
  for each non-culled graphics pass:
    Execute(RenderPassJob, &recordCounter, "RGPassRecord")
  WaitForCounter(&recordCounter)            // single yield

Phase 2 (serial, primary cmd):
  for each non-culled pass:
    vkCmdPipelineBarrier2
    if compute: pass.execute(primary)
    else:       BeginRendering + ExecuteCommands(secondary) + EndRendering
```

When `FrameDebugger` is in `CaptureRequested` state, `RenderPipeline`
flips `RG::SetSerialize(true)` for every view's RG; phase 1 falls back
to per-pass dispatch+wait so the FrameDebugger's shared
`capturedFrame.passes`/`drawCalls` vectors don't race.

## Deferred

- **Within-pass parallel recording** (Tier 2). The per-pass job still
  records its own secondary cmd buffer serially. Full intra-pass
  parallelism (multiple workers recording draws into one secondary)
  is a future epic.
- **Frame-0 render-stage skip** (spec D5). Shipped impl runs frames 0/1
  sync against `Current()`; the literal "skip frame 0 render" path
  needs a backend `TimelineSemaphore::Init(1)` + `AcquireImage` skip
  outside this epic's scope.
