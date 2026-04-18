# v1.6.0 — arch-cleanup

**Date:** 2026-04-18
**Commits:** 8
**Issue:** [#76](https://github.com/Hekbas/Luth/issues/76)

---

## Overview

Phase 1–2 of the architecture refactor. Low-risk mechanical moves that clean up folder misalignment, preparing the tree for the larger `arch-renderer-split` and `arch-target-split` epics. No behavior change — all work is structural.

See the multi-epic plan: [`docs/development/ARCH-REFACTOR-PLAN.md`](../../ARCH-REFACTOR-PLAN.md).

---

## Sub-Tasks

| # | Sub-task | Commit |
|---|---|---|
| A | Extract `events/` from `platform/` | `refactor(events): extract event types from platform/` |
| B | Disperse `utils/` into editor/core/resources | `refactor(utils): disperse utils/ into editor/core/resources` |
| C | Move `FrameData.h` from renderer to core | `refactor(core): move FrameData from renderer to core` |
| D | Rename `Systems`→`SystemRegistry`, fix ownership | `refactor(scene): rename Systems->SystemRegistry, fix ownership` |
| E | Split `Components.h` into `components/` subfolder | `refactor(scene): split Components.h into components/ subfolder` |
| F | Normalize POD component field naming | `refactor(scene): normalize POD component field naming` |
| G | Subdivide `renderer/` into concept folders | `refactor(render): subdivide renderer/ into concept folders` |
| H | Extract `LightTypes.h` from `RenderingSystem` | `refactor(render): extract LightTypes from RenderingSystem` |

---

## Directory Changes

### New folders
- `events/` — extracted from `platform/`
- `editor/widgets/` — from `utils/` (Icons, ImGuiUtils)
- `scene/components/` — granular component headers
- `renderer/resources/` — Buffer, Mesh, Model, Texture
- `renderer/material/` — Material, MaterialSystem
- `renderer/shader/` — Shader, ShaderCompiler, ShaderLibrary
- `renderer/pipeline/` — PipelineManager
- `renderer/lighting/` — IBLPrecompute, LightTypes (new)
- `renderer/settings/` — GTAOSettings, PostProcessSettings
- `renderer/draw/` — DrawCommand

### Removed folders
- `utils/` — dispersed

### Renamed
- `scene/System.h` → `scene/systems/ISystem.h` (class `System` → `ISystem`)
- `scene/Systems.{h,cpp}` → `scene/systems/SystemRegistry.{h,cpp}` (class `Systems` → `SystemRegistry`)
- `utils/LuthIcons.h` → `editor/widgets/Icons.h`
- `utils/ImGuiUtils.h` → `editor/widgets/ImGuiUtils.h`
- `utils/CustomFormatters.h` → `core/LogFormatters.h`
- `utils/ImageUtils.cpp` → `resources/ImageUtils.cpp`
- `renderer/FrameData.h` → `core/FrameData.h`

### Moved
- 25 files from `renderer/` top level into concept subfolders (sub-task G)
- 7 event files from `platform/` to `events/` (sub-task A)

---

## Key Design Decisions

### `SystemRegistry` ownership fix
Previous `Systems::AddSystem<T>()` emplaced a `unique_ptr<T>` into a `vector<shared_ptr<System>>` — implicit conversion masked an ownership-model bug. Fixed:
- Storage: `vector<unique_ptr<ISystem>>` (manager is sole owner)
- `GetSystem<T>()` returns non-owning `T*` (was `shared_ptr<T>`)
- All 5 panel members updated from `shared_ptr<RenderingSystem>` to raw `RenderingSystem*`

### POD component field naming
`struct ID { UUID Value; }` chosen over `struct ID { UUID ID; }` to avoid struct-name/member-name shadow collision. `Value` applied uniformly across `ID`, `Tag`, `Parent`, `Children` (newtype wrapper convention). 14 caller files updated via `.m_X` → `.Value`.

### `Components.h` umbrella
Split into 6 granular headers (`Common`, `Transform`, `Camera`, `Rendering`, `Lights`, `Animation`) but kept `Components.h` as a 7-line umbrella `#include`. Existing `#include "luth/scene/Components.h"` callsites required no changes.

### `LightTypes.h` extraction
`DirectionalLightData`, `PointLightData`, `LightUniforms`, `k_ShadowCascadeCount`, `k_ShadowResolution` moved from `RenderingSystem.h` (which includes Vulkan + scene + renderer headers) to a pure-data header with only `core/LuthTypes.h` + `glm.hpp` as dependencies. Shader reflection, tests, and future tools can include freely.

---

## Lessons

**Bulk text rewrites: use `perl` with `BEGIN{binmode}`, not `sed`.**
`sed -i` on Git Bash for Windows silently strips CRLF → LF on every file it touches, even files with no match — produced 170+ bogus "modified" entries in `git status` during sub-task A. `perl -i -pe 'BEGIN{binmode(ARGV);binmode(STDOUT);} s|...|...|g'` reads/writes in binary mode and preserves CRLF byte-for-byte.

**Ownership bugs hide behind implicit conversions.** The `unique_ptr`→`shared_ptr` container mismatch in `Systems` compiled cleanly because `vector::emplace_back` accepts anything convertible to the element type. Worth grepping for "raw pointer returned from smart container" patterns as a class of future bugs.

---

## Build Verification

- Debug x64 builds clean after every sub-task (8 incremental builds)
- Premake regeneration clean (`scripts\setup\setup_windows.bat` equivalent)
- No new warnings
- `Luth.lib` + `Luthien.exe` artifacts produced
