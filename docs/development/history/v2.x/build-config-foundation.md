# v2.8.5 — build-config-foundation

**Date:** 2026-04-27
**Commits:** 1 (on `refactor/build-config-foundation`)
**Issue:** [#97](https://github.com/Hekbas/Luth/issues/97)

---

## Overview

Centralized build-configuration into a single header `luth/core/BuildConfig.h` and renamed the premake-exported configuration macros to project-namespaced equivalents (`DEBUG`/`RELEASE`/`DIST` → `LUTH_BUILD_DEBUG`/`LUTH_BUILD_RELEASE`/`LUTH_BUILD_DIST`). Engine code no longer tests toolchain-specific macros (`_DEBUG`/`NDEBUG`) or unsafe bare names (`DEBUG`/`RELEASE`/`DIST`) directly. The motivating bug — Vulkan validation layers leaking into Release builds — was a side effect of `VulkanContext.h` testing those toolchain macros instead of an engine-owned signal.

Internal foundation work with no user-facing behavior change. First effort under the new "tag every effort, GitHub Release for milestones only" policy (captured in `CLAUDE.md` and the `reference_epic_wrapup` memory in the same effort).

---

## Root cause of the validation leak

The original gate in `VulkanContext.h` read:

```cpp
#if defined(LUTH_ENABLE_VALIDATION)
    bool m_EnableValidationLayers = (LUTH_ENABLE_VALIDATION != 0);
#elif defined(_DEBUG) || !defined(NDEBUG)
    bool m_EnableValidationLayers = true;
#else
    bool m_EnableValidationLayers = false;
#endif
```

In MSVC Release with premake's `runtime "Release"`:

- `_DEBUG` is **not** auto-defined (release CRT, `/MD` not `/MDd`).
- `NDEBUG` is **not** auto-defined either — premake's `optimize "on"` does not add it.
- Therefore `!defined(NDEBUG)` evaluated to `true` → validation layers enabled in Release.

Testing `_DEBUG`/`NDEBUG` directly is a contract with the toolchain that varies across MSVC/Clang/GCC and across build-system versions. AAA engines route every gate through an engine-owned config layer for exactly this reason — Unreal's `Runtime/Core/Public/Misc/Build.h` derives `WITH_EDITOR`/`DO_CHECK`/`STATS` from `UE_BUILD_*`; Source 2's `tier0/platform.h` does the same with `IsRetail()`/`IsCert()`. Engine code never tests compiler builtins.

---

## Design

Two-layer model:

1. **Build configuration (set by premake)** — exactly one of `LUTH_BUILD_DEBUG` / `LUTH_BUILD_RELEASE` / `LUTH_BUILD_DIST` per configuration. A `#error` in `BuildConfig.h` guards against all of them being unset.
2. **Derived feature flags** — `LUTH_ENABLE_VALIDATION`, `LUTH_SPIRV_CROSS_ENABLED`. Each has a default by build config and is overridable on the command line with `-DLUTH_ENABLE_X=0|1` (or in premake via `defines { "LUTH_ENABLE_X=1" }`).

Engine code consumes the derived flags, never the build-config layer directly when there's a feature-flag equivalent. Toolchain macros (`_DEBUG`, `NDEBUG`, bare `DEBUG`/`RELEASE`/`DIST`) are no longer tested anywhere in first-party engine or editor code. The header is included via `luthpch.h`, so engine + editor TUs see it through their PCH. Runtime code (no PCH) tests `LUTH_BUILD_DEBUG` directly in the one place it matters (`EntryPoint.h`'s "press Enter to exit" pause).

```cpp
// luth/source/luth/core/BuildConfig.h (excerpt)
#if !defined(LUTH_BUILD_DEBUG) && !defined(LUTH_BUILD_RELEASE) && !defined(LUTH_BUILD_DIST)
    #error "No LUTH_BUILD_* configuration defined — premake5.lua must set one"
#endif

#if !defined(LUTH_ENABLE_VALIDATION)
    #if defined(LUTH_BUILD_DEBUG)
        #define LUTH_ENABLE_VALIDATION 1
    #else
        #define LUTH_ENABLE_VALIDATION 0
    #endif
#endif
```

---

## Changes

| File | Change |
|---|---|
| `luth/source/luth/core/BuildConfig.h` | NEW — build-config detection + `LUTH_ENABLE_VALIDATION` + `LUTH_SPIRV_CROSS_ENABLED` |
| `luth/source/luthpch.h` | Add `#include "luth/core/BuildConfig.h"` near top so all engine + editor TUs see it via PCH |
| `luth/premake5.lua` | `DEBUG`/`RELEASE`/`DIST` → `LUTH_BUILD_*`; drop `LUTH_SPIRV_CROSS_ENABLED=1` (now in header) |
| `luthien/premake5.lua` | Same rename |
| `runtime/premake5.lua` | Same rename |
| `luth/source/luth/renderer/backend/vulkan/VulkanContext.h` | Collapse 7-line `#if/#elif/#else` gate to `bool m_EnableValidationLayers = (LUTH_ENABLE_VALIDATION != 0);` |
| `luth/source/luth/core/EntryPoint.h` | `#ifdef _DEBUG` → `#if defined(LUTH_BUILD_DEBUG)` |
| `luth/source/luth/core/diagnostics/Log.h` | `_DEBUG \|\| DEBUG` → `LUTH_BUILD_DEBUG` |
| `luthien/source/luthien/inspectors/component_drawers/DebugDrawers.cpp` | `DEBUG` → `LUTH_BUILD_DEBUG` |
| `luthien/source/luthien/inspectors/component_drawers/RegisterComponentDrawers.cpp` | `DEBUG` → `LUTH_BUILD_DEBUG` |

Single implementation commit: `6a6e019`.

---

## Out of scope (deliberately)

- **Gating `LH_CORE_ASSERT` by build config.** Behavioral change — needs its own decision (do we want asserts in Release? Dist?). Currently always-on in all configs.
- **Moving Tracy defines (`TRACY_ENABLE`, `TRACY_FIBERS`, `TRACY_ON_DEMAND`) into `BuildConfig.h`.** Those configure the *Tracy library*, not the engine. They belong in premake.
- **Adding `LUTH_PLATFORM_*` / `LUTH_COMPILER_*`.** Engine is single-target (Windows + MSVC). Add when a second target lands.
- **Speculative feature flags** (`LUTH_WITH_EDITOR`, `LUTH_LOG_LEVEL`, etc.) — added per-need, not pre-emptively. The MVP foundation has just the two flags that have actual call sites today.

`Fiber.h:124` retains `#ifndef NDEBUG` — that's the standard C-library idiom for pairing with `assert()` from `<cassert>`, not a build-config gate. Untouched.

---

## Build verification

Solution regenerated from `extern/premake/windows/premake5.exe`. All three configurations build clean:

- **Debug x64** — no new warnings; runtime confirms `VK_LAYER_KHRONOS_validation` messages still appear (sanity check that we didn't kill validation everywhere).
- **Release x64** — no new warnings; runtime confirms **no** validation messages (the bug fix).
- **Dist x64** — no new warnings.

Sweep confirms no first-party engine/editor code tests `_DEBUG`, bare `DEBUG`, bare `RELEASE`, or bare `DIST` after the migration. `NDEBUG` survives only at `Fiber.h:124` (assert-pairing idiom, intentional).

---

## Workflow change shipped alongside

Mid-effort, an architecture conversation surfaced that the project's "release every effort" cadence was conflating tags (cheap archaeology) with GitHub Releases (curated user-facing changelog). Updated the policy:

- **Tag every effort.** Cheap rollback points (`git checkout v2.8.5`), pairs with a history file. Always.
- **Publish a GitHub Release for *milestones* only** — MINOR/MAJOR version bumps or user-facing changes. Internal refactors, polish, and chores tag-only.
- **Rule of thumb:** would a portfolio reviewer / hypothetical user care? → Release. Pure internal cleanup? → Tag only.

Captured in `CLAUDE.md` (renamed "Release notes — two audiences, two artifacts" → "Tagging vs. releasing" + "History file vs. Release body") and in the `reference_epic_wrapup` memory entry. This effort is the first under the new policy: tagged `v2.8.5`, history file written (this document), no `gh release create`. Future internal refactors (`frame-debugger-polish`, `animation-quick-pass`) follow the same pattern; `jolt-physics` (v2.9.0) will be the first published Release under the new rules.
