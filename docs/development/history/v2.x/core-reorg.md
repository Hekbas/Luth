# v2.3.0 — core-reorg

**Date:** 2026-04-18
**Commits:** 3 (on `epic/core-reorg`)
**Issue:** [#81](https://github.com/Hekbas/Luth/issues/81)

---

## Overview

Third epic of the post-v2.0 architecture-review series. Reorganized `luth/source/luth/core/` from a flat 20-file folder spanning 9 unrelated domains into three semantic sub-folders — `types/`, `diagnostics/`, `time/`. Split the overloaded `LuthTypes.h` (primitives + GLM aliases + traits + ostream formatters) into focused single-purpose headers. `Math.h` renamed to `types/LuthMath.h`, finally living next to its sibling type aliases. Pure file-organization refactor — zero runtime behavior change.

Minor version bump to **v2.3.0** per the ROADMAP MINOR rule (one completed epic with engineering-visible changes — first reorg of the core/ folder since the v1.6 `arch-cleanup` series; the foundation primitives layer is now properly grouped).

---

## Sub-Tasks

| # | Sub-task | Commit |
|---|---|---|
| A | Spec scaffold | `9c5af8f docs(epic): add core-reorg spec` |
| B | Reorganize core/ subfolders + include rewrite | `31abb61 refactor(core): reorganize core/ into types/, diagnostics/, time/ subfolders` |
| C | Docs + v2.3.0 + history + wrap-up | `chore(release): core-reorg → v2.3.0` |

(Three commits total — the move + include rewrite is one atomic unit because partial moves break the build.)

---

## Target layout

```
luth/source/luth/core/
├── App.{h,cpp}              top-level lifecycle
├── EntryPoint.h
├── Version.{h,cpp}
├── FrameData.h              triple-buffer frame state
├── UUID.{h,cpp}
├── EditorHooks.{h,cpp}      engine→editor bridge
├── ProjectFile.{h,cpp}      .luthproj IO
├── types/
│   ├── LuthTypes.h          primitives only — i8…u64, f32/f64, byte, fs alias, sizeof asserts
│   ├── LuthMath.h           Vec/IVec/UVec/Mat/Quat aliases + sizeof asserts + Math:: facade + Assimp/AABB/Frustum helpers
│   └── TypeTraits.h         IsGLMVector / IsGLMMatrix
├── diagnostics/
│   ├── Log.{h,cpp}          spdlog wrapper + macros
│   ├── LogFormatters.h      fmt formatters (STL + Vulkan) + ostream<< Vec3/Mat4
│   └── Profiler.h           Tracy macros (TRACY_ENABLE-gated)
└── time/
    ├── Time.h               static engine clock + delta time
    └── Timer.h              scoped stopwatch
```

---

## What changed (file by file)

### Splits

- **`LuthTypes.h` → `types/LuthTypes.h` + `types/LuthMath.h` + `types/TypeTraits.h` + `diagnostics/LogFormatters.h`**
  - Primitives (i8…u64, f32/f64, byte, `namespace fs`, sizeof asserts on `i32`/`f32`) → `types/LuthTypes.h`
  - GLM type aliases (`Vec/IVec/UVec/Mat/Quat`) + their sizeof asserts → `types/LuthMath.h`
  - `IsGLMVector` / `IsGLMMatrix` template traits → `types/TypeTraits.h`
  - `operator<<(std::ostream&, const Vec3&)` and `operator<<(std::ostream&, const Mat4&)` → `diagnostics/LogFormatters.h` (logging concern, not a type concern)
  - Dead `Luth::Normalize<T>` and `Luth::Cross<T>` forward decls **deleted** — no definitions, no callers, superseded by `Math::Normalize` / `Math::Cross` in the v2.2 facade

### Renames

- `Math.h` → `types/LuthMath.h` (merged with the GLM aliases extracted from LuthTypes.h)
- `Log.{h,cpp}` → `diagnostics/Log.{h,cpp}`
- `LogFormatters.h` → `diagnostics/LogFormatters.h` (now also owns the Vec3/Mat4 ostream operators)
- `Profiler.h` → `diagnostics/Profiler.h`
- `Time.h` → `time/Time.h`
- `Timer.h` → `time/Timer.h`

### Stays at `core/` root

`App`, `EntryPoint`, `Version`, `FrameData`, `UUID`, `EditorHooks`, `ProjectFile` — top-level engine lifecycle. None share a domain with another; nesting them serves no readability gain.

---

## Migration

### Bulk include rewrite

132 caller files rewritten via `perl -pi`. Universal swaps (one regex per old path → new path):

| Old path | New path |
|---|---|
| `luth/core/Math.h` | `luth/core/types/LuthMath.h` |
| `luth/core/Log.h` | `luth/core/diagnostics/Log.h` |
| `luth/core/LogFormatters.h` | `luth/core/diagnostics/LogFormatters.h` |
| `luth/core/Profiler.h` | `luth/core/diagnostics/Profiler.h` |
| `luth/core/Time.h` | `luth/core/time/Time.h` |
| `luth/core/Timer.h` | `luth/core/time/Timer.h` |

`luth/core/LuthTypes.h` got a **per-file conditional rewrite** because the old single header served two distinct audiences:

| File uses Vec/IVec/UVec/Mat/Quat token? | New include |
|---|---|
| Yes (18 files) | `luth/core/types/LuthMath.h` (gives them aliases + facade + primitives transitively) |
| No (48 files) | `luth/core/types/LuthTypes.h` (primitives only — no glm pulled) |

Detection regex: `\b(Vec[234]|IVec[234]|UVec[234]|Mat[234]|Quat)\b` — word-boundary anchored to avoid false matches on `Material`, `Vector`, etc.

### PCH update

`luthpch.h` reroutes its 5 core/ includes to the new sub-folder paths. Premake uses `source/**.h` recursive globs, so subfolders pick up automatically — no `premake5.lua` change needed. VS Solution Explorer reflects the new folder tree on next regen.

`lepch.h` (editor PCH) needed no change — it just `#include "luthpch.h"`, riding the engine PCH transparently.

---

## Final tally

| Metric | Before | After |
|---|---:|---:|
| Files at `core/` root | 20 | 13 (7 lifecycle files + 6 subfolders + nothing else) |
| Files in `core/types/` | 0 | 3 |
| Files in `core/diagnostics/` | 0 | 3 |
| Files in `core/time/` | 0 | 2 |
| Files including `luth/core/Math.h` | 12 | 0 (all → `types/LuthMath.h`) |
| Files including `luth/core/LuthTypes.h` | 66 | 0 (split per-file) |
| Dead forward decls (`Luth::Normalize`/`Cross`) | 2 | 0 (deleted) |

---

## Key Design Decisions

### Atomic move + include rewrite

Sub-task B is one commit, not three (per-folder). Rationale: the PCH pulls every relocated header. A partial move (e.g., `types/` first, leave `diagnostics/` for a second commit) would force the PCH to point at half-old / half-new paths during the intermediate commit, which is awkward and error-prone. Atomic move keeps every commit on the branch independently buildable.

### LuthTypes.h conditional split

Two viable approaches were considered:
- **Route everything to LuthMath.h** — simpler perl, but every primitives-only file pulls glm headers (compile-time penalty negligible thanks to PCH, but it's looser than needed).
- **Per-file inspection** — picks the minimal include for each consumer.

Chose the second. The split has real semantics: `core/types/LuthTypes.h` now means "I need primitives, not math." A `Fiber.h` or `JobSystem.h` that includes only `LuthTypes.h` documents that intent at the include site. The cost (one extra grep per file) is paid once during this epic.

### Drop Normalize / Cross forward decls

Two template forward decls in the old `LuthTypes.h`:
```cpp
template<typename T> T Normalize(const T& v);
template<typename T> T Cross(const T& a, const T& b);
```
Had no definitions anywhere. Linker would fail if anyone called `Luth::Normalize(...)` directly. They were leftover from before the math facade existed (v2.2.0 introduced `Math::Normalize` / `Math::Cross` as the canonical wrappers). Per the math-abstraction lesson "delete dead code aggressively as we pass through it" — this epic was the right time to remove them.

### Keep `IsGLMVector` / `IsGLMMatrix` traits

Currently unused in the engine (grep confirms). Could have been deleted as dead code, but the math-abstraction history flagged them as foundational ("Renaming the specialization body would break the abstraction at its foundation"). Preserved in `types/TypeTraits.h` for future SFINAE consumers (e.g., a future serializer that needs to dispatch on glm types).

### LogFormatters.h as the ostream<< home

The `operator<<` overloads for `Vec3`/`Mat4` exist purely so spdlog/fmt's ostream adapter can format them in `LH_CORE_INFO("{}", vec3)`. They're a logging concern, not a type concern. Moving them to `LogFormatters.h` (which already houses fmt formatters for STL + Vulkan types) keeps the abstraction clean: `LuthMath.h` is now headers-only with no `<spdlog/fmt/ostr.h>` baggage.

### `core/` root vs subfolders

Files that don't share a domain with another file stay at the root. `App`, `Version`, `EntryPoint`, `FrameData`, `UUID`, `EditorHooks`, `ProjectFile` — each is a singleton concept; nesting them under `lifecycle/` or `engine/` would invent a category for organization's sake without reader benefit. The reorg targets the actually-grouped concerns (types, diagnostics, time).

---

## Build Verification

- 3 commits on `epic/core-reorg`; every commit builds Debug x64 + Release x64 clean.
- Only pre-existing warnings present (LNK4006 from vulkan-1.lib transitively colliding with shaderc/ws2_32/dbghelp; C4996 from `getenv` / `strncpy` in editor; C4244 chrono narrowing in `Editor.cpp:420`).
- `rg "luth/core/(LuthTypes|Math|Log|Profiler|Time|Timer|LogFormatters)\.h"` matches only docs (this history file).

---

## Lessons

**Atomic moves keep `git bisect` clean.** Splitting the move across "create new files" → "update PCH" → "delete old files" → "rewrite callers" would have introduced 3 build-broken intermediate commits. Per math-abstraction's "every commit builds standalone" principle: bundle the move into one commit, accept the larger diff, get a bisect-friendly history in exchange.

**Per-file conditional rewrites are cheap when the categories have a clean syntactic signal.** `Vec[234]|IVec[234]|UVec[234]|Mat[234]|Quat` is a precise enough regex (the digit/specific suffix avoids false positives like `Material` or `Vector`) that a simple `grep -qE … && perl -pi …` shell loop nailed all 66 files in seconds. No manual triage needed. The same approach didn't work for the math facade rewrite (function names like `glm::translate` had no clean syntactic peer to disambiguate from non-glm `translate`), but it works perfectly for type-name rewrites.

**Dead code lingers across abstraction passes.** `Luth::Normalize` / `Cross` forward decls survived the math-abstraction epic untouched even though it built the canonical `Math::Normalize` / `Math::Cross` replacements. Reason: nobody was calling them, so neither pass nor build noticed. Pass-through cleanups during reorgs are the natural moment to surface this kind of leftover — the simplify principle pays off whenever you touch an old file for an unrelated reason.
