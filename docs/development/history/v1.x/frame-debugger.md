# Phase 5-E: Frame Debugger ✅ (2026-03-22)

**Goal:** Unity-style Frame Debugger panel showing all render graph passes, GPU timing, pipeline state, and intermediate texture previews.

### Architecture

The RenderGraph is stack-local (rebuilt each frame), so `RenderGraphSnapshot` captures per-frame pass/resource metadata as lightweight POD structs that persist for UI display. `GPUTimerPool` manages triple-buffered `VkQueryPool` timestamps with 2-frame readback latency (GPU N-2 guaranteed complete). Timer injection in `RenderGraph::Execute()` wraps each non-culled pass with `TOP_OF_PIPE`/`BOTTOM_OF_PIPE` timestamps.

### Key Design Decisions

- **Split-panel layout**: Left panel = resizable pass tree with GPU timing; Right panel = output texture preview + pipeline state table + resource list
- **Named texture registry**: Maps pass outputs (SceneColor, ShadowMap, BloomA/B, LDR) to ImGui-compatible texture IDs for preview
- **Depth format detection**: `IsDepthFormat()` check prevents null-sampler crash when previewing depth-only passes
- **Viewport resize guard**: `width <= 16384 && height <= 16384` prevents unsigned underflow from negative ImGui regions

### Files

| File | Changes |
|---|---|
| `luth/renderer/backend/vulkan/GPUTimerPool.h/.cpp` | NEW — VkQueryPool timestamp management |
| `luth/renderer/rendergraph/RenderGraphSnapshot.h` | NEW — PassSnapshot, ResourceSnapshot structs |
| `luth/editor/panels/FrameDebuggerPanel.h/.cpp` | NEW — Split-panel UI with event slider |
| `luth/renderer/rendergraph/RenderGraph.h/.cpp` | Timer injection in Execute() |
| `luth/renderer/Renderer.h/.cpp` | Forward GPUTimerPool* to ExecuteGraph |
| `luth/scene/systems/RenderingSystem.h/.cpp` | CaptureSnapshot, timer plumbing, named textures |
| `luth/editor/Editor.cpp` | Panel registration |
