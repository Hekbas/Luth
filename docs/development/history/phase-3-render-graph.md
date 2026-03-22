# Phase 3: Render Graph Refactor ✅ (2026-03-07)

| File | Changes |
|---|---|
| [RenderGraph.h](file:///c:/Users/Hekbas/CITM/5_TFG/Luth/luth/source/luth/renderer/rendergraph/RenderGraph.h) | Removed `ExecuteParallel()`, added `Execute(primaryCmd)`, dead-pass culling fields, lifetime tracking |
| [RenderGraph.cpp](file:///c:/Users/Hekbas/CITM/5_TFG/Luth/luth/source/luth/renderer/rendergraph/RenderGraph.cpp) | 3-step `Compile()` (Cull → Lifetimes → Barriers), serial `Execute()` with batched barriers + parallel inner recording |
| [RenderPassJob.h](file:///c:/Users/Hekbas/CITM/5_TFG/Luth/luth/source/luth/renderer/backend/vulkan/RenderPassJob.h) | Removed `TargetFrame` push — executor reads `CommandBuffer` directly after `WaitForCounter` |
| [Renderer.cpp](file:///c:/Users/Hekbas/CITM/5_TFG/Luth/luth/source/luth/renderer/Renderer.cpp) | Simplified `ExecuteGraph` to just `graph.Execute(primaryCmd)` |

### Execution Model
```
For each non-culled pass (in order):
  1. Batch all pre-barriers → single vkCmdPipelineBarrier2
  2. Dispatch RenderPassJob (records secondary cmd buffer on worker)
  3. WaitForCounter (V5 inline execution if depth allows)
  4. BeginRendering → ExecuteCommands → EndRendering on primary
```
Parallelism comes from **within** a pass. Inter-pass ordering is serial (correct for barrier dependencies).

### Commit
```
refactor(rendergraph): serial pass execution with batched barriers

- Compile() now: CullDeadPasses → ComputeLifetimes → SolveBarriers
- Execute() replaces ExecuteParallel: serial pass iteration,
  batched barriers per pass, parallel inner recording via RenderPassJob
- RenderPassJob simplified: no TargetFrame push, executor reads directly
- Renderer::ExecuteGraph delegates to graph.Execute(primaryCmd)
- Resource lifetime tracking (firstPass/lastPass) for future aliasing
```
