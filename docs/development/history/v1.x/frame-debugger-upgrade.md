# Phase 9 — Frame Debugger Upgrade

**Date:** 2026-04-03
**Goal:** Upgrade FrameDebuggerPanel from pass-level summaries to Unity-style per-draw-call stepping with trigger-based frame capture and visual scrubbing.

---

## Motivation

The existing frame debugger (Phase 5-E) showed GPU timing per pass and a render graph snapshot, but nothing below pass granularity. Debugging incorrect draw order, wrong pipeline state, or bad transforms required printf-level guesswork. The goal was Unity's Frame Debugger UX: click Enable, frame freezes, scrub through every individual draw call watching the scene composite one mesh at a time.

---

## Architecture Decisions

### Trigger-based capture, not per-frame recording

Recording every draw call every frame would be expensive and unnecessary. Instead:
- `RequestCapture()` sets state to `CaptureRequested`
- On the next `Update()`, draw calls are captured to `m_CapturedFrame` as they execute
- After `ExecuteGraph()`, state transitions to `Frozen` and the captured draw vectors are copied
- While Frozen, `Update()` calls `RenderCapturedFrame(m_DebuggerDrawLimit)` instead of the real frame

### CPU-side command recording, not Vulkan wrapping

Capturing at the `DrawCommand` / `DrawBatch` level rather than wrapping `vkCmdDrawIndexed` keeps the implementation simple and avoids re-entrant secondary command buffer complexity. The render graph re-records everything from scratch on each scrub tick.

### Resource lifetime during freeze

`DrawCommand` holds `shared_ptr<Model>`, keeping GPU vertex/index buffers alive for the duration of the freeze. The captured draw vectors (`m_CapturedOpaqueDraws`, `m_CapturedCutoutDraws`, `m_CapturedTransparentDraws`) are copied by value from the live draw lists.

---

## New Data Structures — `FrameCapture.h`

```
rendergraph/FrameCapture.h
```

- `CapturedPipelineState` — shader name, renderMode (u32), cullMode (u32), polygonMode, skinned/depthTest/depthWrite/blend flags
- `CapturedDrawCall` — globalIndex, passLocalIndex, passIndex, passName, meshName, entityName, index count, modelMatrix, materialIndex, shadeMode, entityID, boneOffset, pipelineState
- `CapturedPass` — name, firstDrawIndex, drawCallCount, gpuTimeMs, pipelineState, activeRenderTarget, isDepthTarget
- `CapturedFrame` — flat vector of draw calls + vector of passes + resource snapshots

Used `u32` for renderMode/cullMode to avoid including `Material.h` from the rendergraph directory (circular dependency prevention).

---

## State Machine — `RenderingSystem`

```
enum class DebuggerState : u8 { Inactive, CaptureRequested, Frozen }
```

| State | Behavior |
|-------|----------|
| Inactive | Normal frame rendering |
| CaptureRequested | Normal render + capture instrumentation active; transitions to Frozen after ExecuteGraph |
| Frozen | Calls RenderCapturedFrame() each frame; real draw lists not rebuilt |

Public API added:
- `RequestCapture()` / `ExitCapture()`
- `GetDebuggerState()` / `GetCapturedFrame()`
- `SetDebuggerDrawLimit(u32)` / `GetDebuggerDrawLimit()`

---

## Capture Instrumentation

Every draw-emitting function was wrapped with `BeginCapturePass` / `CaptureDrawCall` / `EndCapturePass` guards — all no-ops unless `m_DebuggerState == CaptureRequested`:

| Pass | Notes |
|------|-------|
| ShadowPass | Per-entity CaptureDrawCall after each vkCmdDrawIndexed; mesh name via `model->GetName()` |
| GeometryPass | Inside DrawBatch lambda after vkCmdDrawIndexed; entity name resolved via m_EntityLookup + Component::Tag |
| Skybox | Single draw entry |
| Bloom Extract / Blur | Treated as 3 fixed draws |
| PostProcess | Single draw entry |
| Outline / SelectionMask | Single draw entries |
| ImGui | Single draw entry; always replayed |

`CapturedPass::activeRenderTarget` tracks which named texture is the active output at each pass (SceneColor, ShadowMap, LDROutput, etc.) and `isDepthTarget` flags depth-format targets for shader selection.

---

## Frame Re-Recording — `RenderCapturedFrame()`

On each scrub tick, rebuilds the full render graph from captured data:

1. Determines which pass is "active" based on `maxDrawCalls` position
2. Replays ShadowPass draws via `ReplayShadowDraws` lambda with counter check
3. Replays GeometryPass draws via `DrawBatchReplay` lambda with counter check
4. Replays Skybox if counter allows
5. Replays Bloom + PostProcess if counter allows
6. **Rescue blit** if PostProcess not reached — tone-maps HDR SceneColor or linearizes ShadowMap depth to LDROutput
7. Always adds ImGui pass

`m_ReplayDrawCounter` is reset to 0 before each re-record and incremented per draw.

---

## Edge Cases

### Active render target tracking
Each `CapturedPass` stores `activeRenderTarget` (the name in `m_NamedTextures`) so the scrubber always knows what texture to blit when the user stops mid-frame.

### Rescue blit
When scrubbing stops before PostProcess executes, the HDR `SceneColor` or depth `ShadowMap` would never reach `LDROutput`. `AddDebugBlitPass()` adds a final render graph pass that:
- Reads the active render target
- Runs either `debugBlit.frag` (Reinhard tonemapping + gamma) or `debugDepth.frag` (near/far linearization)
- Writes to `LDROutput` for normal swapchain presentation

### Depth visualization
`debugDepth.frag` linearizes the non-linear depth buffer using push-constant near/far planes and outputs inverted greyscale (close = bright). This makes shadow pass scrubbing readable.

---

## New Shaders

| File | Purpose |
|------|---------|
| `sandbox/assets/shaders/debugBlit.frag` | HDR→LDR (Reinhard + gamma), set=0 binding=0 combined sampler |
| `sandbox/assets/shaders/debugDepth.frag` | Depth linearization, push constants: float nearPlane, float farPlane |

Both use `fullscreen.vert` (existing).

---

## Panel Rewrite — `FrameDebuggerPanel`

Split into two modes dispatched from `OnRender()`:

**Live mode** (Inactive/CaptureRequested): retains original pass tree + GPU timing. "Enable" button calls `RequestCapture()`.

**Capture mode** (Frozen):
- Red "Disable" button calls `ExitCapture()`
- Draw call slider (0 .. totalDrawCalls) + arrow step buttons
- Expandable pass tree with draw call children; clicking updates slider and triggers re-render via `SetDebuggerDrawLimit`
- Detail panel: output preview (GetSceneColor), Draw Call info table, Pipeline State table (from captured values), Transform (decomposed modelMatrix via glm::decompose), Push Constants table

---

## Compilation Fixes

Four issues discovered at build time:
- `model->m_Path` (protected member) → `model->GetName()` (public Asset API)
- `VK_CULL_MODE_FRONT_BIT` narrowing to `u32` → `static_cast<u32>()`
- `PipelineConfig::depthTestEnable`/`depthWriteEnable` (wrong names) → `depthTest`/`depthWrite`
- Shadow replay lambda missing `maxDrawCalls` capture → `[this, maxDrawCalls]`

---

## Files Modified / Created

| File | Change |
|------|--------|
| `luth/source/luth/renderer/rendergraph/FrameCapture.h` | **New** — capture data structures |
| `luth/source/luth/scene/systems/RenderingSystem.h` | Added DebuggerState, public API, private helpers, member vars |
| `luth/source/luth/scene/systems/RenderingSystem.cpp` | ~700 lines added — instrumentation, state machine, RenderCapturedFrame, InitDebugBlitResources, AddDebugBlitPass |
| `luth/source/luth/editor/panels/FrameDebuggerPanel.h` | Rewritten — live/capture mode split |
| `luth/source/luth/editor/panels/FrameDebuggerPanel.cpp` | Rewritten — ~560 lines, full per-draw-call UI |
| `sandbox/assets/shaders/debugBlit.frag` | **New** — rescue blit shader |
| `sandbox/assets/shaders/debugDepth.frag` | **New** — depth visualization shader |
