# Phase 11 — Architecture Cleanup

**Date:** 2026-04-13
**Version:** v1.1.1
**Commits:** 6

---

## Overview

Large-scale refactor to reduce RenderingSystem.cpp from 4,060 to ~2,321 lines and break apart monolithic files into focused, maintainable units. No behavioral changes — pure mechanical extraction with one pre-existing resource leak fix.

---

## Commit Breakdown

### Commit 1: Extract EditorCamera

Moved `EditorCamera` class out of `ScenePanel` into its own files.

| File | Action |
|------|--------|
| `editor/EditorCamera.h` | NEW — class declaration |
| `editor/EditorCamera.cpp` | NEW — orbit/fly input, frustum, all method implementations |
| `editor/panels/ScenePanel.h` | EDIT — removed class definition, added `#include` |
| `editor/panels/ScenePanel.cpp` | EDIT — removed EditorCamera method bodies |

### Commit 2: Decouple RenderingSystem from Editor via CameraParams

Removed direct `#include "Editor.h"` / `EditorSelection.h` / `ScenePanel.h` from RenderingSystem. Editor state is now injected via a `CameraParams` struct set by `App.cpp` each frame before `Update()`.

| File | Action |
|------|--------|
| `renderer/CameraParams.h` | NEW — struct with view/proj/position/IBL settings + selected entity set |
| `scene/systems/RenderingSystem.h` | EDIT — added `SetCameraParams()` setter + `m_CameraParams` member |
| `scene/systems/RenderingSystem.cpp` | EDIT — replaced 5 Editor/EditorSelection call sites with `m_CameraParams` reads |
| `core/App.cpp` | EDIT — constructs and injects `CameraParams` before rendering update |

### Commit 3: Extract IBL Precomputation

Moved ~540 lines of one-shot IBL precomputation (equirect-to-cubemap, irradiance convolution, pre-filtered environment map, BRDF LUT) into a standalone utility.

| File | Action |
|------|--------|
| `renderer/IBLPrecompute.h` | NEW — `IBLResult` struct + `IBL::Precompute()` function |
| `renderer/IBLPrecompute.cpp` | NEW — all IBL generation logic |
| `scene/systems/RenderingSystem.cpp` | EDIT — `InitIBLResources()` delegates to `IBL::Precompute()` |

### Commit 4: Extract FrameDebugger

Moved frame debugger state machine, capture helpers, and debug blit resources (~16 member variables, 4 methods) into a standalone struct. Fixed a pre-existing Vulkan resource leak (debug blit sampler, descriptor set layout, and descriptor pool were never destroyed).

| File | Action |
|------|--------|
| `renderer/DrawCommand.h` | NEW — `DrawCommand` + `ObjectPushConstants` (breaks circular dep) |
| `renderer/FrameDebugger.h` | NEW — `DebuggerState` enum + `FrameDebugger` struct |
| `renderer/FrameDebugger.cpp` | NEW — BeginCapturePass, EndCapturePass, CaptureDrawCall, Shutdown |
| `scene/systems/RenderingSystem.h` | EDIT — replaced 16 members with `FrameDebugger m_FrameDebugger` |
| `scene/systems/RenderingSystem.cpp` | EDIT — 15 member renames, added `Shutdown()` call in destructor |

### Commit 5: Extract Render Passes into passes/

Moved ~1,120 lines across 9 render pass methods into individual .cpp files under `renderer/passes/`. Methods remain as `RenderingSystem::` members to avoid massive params structs.

| File | Action |
|------|--------|
| `renderer/passes/ShadowPass.cpp` | NEW — 186 lines |
| `renderer/passes/GeometryPass.cpp` | NEW — 302 lines |
| `renderer/passes/SkyboxPass.cpp` | NEW — 92 lines |
| `renderer/passes/BloomPass.cpp` | NEW — 188 lines |
| `renderer/passes/PostProcessPass.cpp` | NEW — 90 lines |
| `renderer/passes/SelectionPass.cpp` | NEW — 208 lines |
| `renderer/passes/OutlinePass.cpp` | NEW — 108 lines |
| `renderer/passes/GridPass.cpp` | NEW — 104 lines |
| `renderer/passes/ImGuiPass.cpp` | NEW — 65 lines |
| `scene/systems/RenderingSystem.cpp` | EDIT — 4,060 → 2,321 lines |

### Commit 6: Split Command.h into Per-Category Headers

Broke the monolithic 375-line `Command.h` into 6 focused headers under `editor/commands/`. Original `Command.h` becomes a backward-compatible umbrella include.

| File | Action |
|------|--------|
| `editor/commands/ICommand.h` | NEW — ICommand base + CompoundCommand |
| `editor/commands/ComponentPropertyCommand.h` | NEW — template class |
| `editor/commands/EntityCommands.h` | NEW — 7 entity command declarations |
| `editor/commands/EntityCommands.cpp` | NEW — CommandUtil + entity command implementations |
| `editor/commands/ComponentCommands.h` | NEW — ComponentAdd/Remove templates |
| `editor/commands/AssetCommands.h` | NEW — MaterialSnapshot + ModelInstantiate declarations |
| `editor/commands/AssetCommands.cpp` | NEW — asset command implementations |
| `editor/Command.h` | EDIT — 375 → 10 lines (umbrella include) |
| `editor/Command.cpp` | EDIT — 3-line stub |

---

## Metrics

| Metric | Before | After |
|--------|--------|-------|
| RenderingSystem.cpp | 4,060 lines | 2,321 lines |
| RenderingSystem.h members | ~110 | ~85 (FrameDebugger consolidation) |
| Command.h | 375 lines | 10 lines (umbrella) |
| New files created | — | 22 |
| Bug fixes | — | 1 (debug blit Vulkan resource leak) |

---

## Key Decisions

1. **Passes stay as RenderingSystem methods** — The plan proposed free functions with params structs, but each pass accesses 5-15 member variables. Extracting into separate `.cpp` files while keeping `RenderingSystem::` membership gave the file organization benefit without creating massive parameter structs or friend classes.

2. **DrawCommand.h to break circular dependency** — `FrameDebugger.h` needs `DrawCommand` and `ObjectPushConstants`, but those were originally in `RenderingSystem.h` which would include `FrameDebugger.h`. Extracting them into a standalone header resolved the cycle.

3. **Umbrella include for backward compatibility** — `Command.h` became a 10-line umbrella include so all 7 existing consumers compile without changes.
