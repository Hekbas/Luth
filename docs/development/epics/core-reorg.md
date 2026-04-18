# Epic: core-reorg

**Issue:** [#81](https://github.com/Hekbas/Luth/issues/81)  |  **Target:** v2.3.0  |  **Est.:** Small (1 day)  |  **Deps:** `math-abstraction` (E3, v2.2.0)

---

## Goal

Reorganize `luth/source/luth/core/` from a flat 20-file folder spanning 9 domains into three semantic sub-folders (`types/`, `diagnostics/`, `time/`). Split the overloaded `LuthTypes.h` (primitives + GLM aliases + traits + ostream formatters) into focused single-purpose headers, and move `Math.h` next to its sibling type aliases under `types/`. Pure file-organization refactor — zero runtime behavior change.

---

## Sub-Tasks and Commit Plan

### A: Spec scaffold

**Commit:** `docs(epic): add core-reorg spec`
**Trailer:** `Part of #81`

| File | Change | Notes |
|------|--------|-------|
| `docs/development/epics/core-reorg.md` | NEW | This file |

---

### B: Reorganize core/ into types/ + diagnostics/ + time/

**Commit:** `refactor(core): reorganize core/ into types/, diagnostics/, time/ subfolders`
**Trailer:** `Part of #81`

Atomic move + include rewrite. Cannot be split — partial moves break the build because `luthpch.h` pulls every relocated header.

**File operations:**

| File | Change | Notes |
|------|--------|-------|
| `luth/source/luth/core/types/LuthTypes.h` | NEW | Primitives only — i8…u64, f32/f64, byte, `namespace fs`, sizeof asserts |
| `luth/source/luth/core/types/LuthMath.h` | NEW | Vec/IVec/UVec/Mat/Quat aliases + sizeof asserts + full `Math.h` content (Math facade + Assimp helpers + AABB + Frustum) |
| `luth/source/luth/core/types/TypeTraits.h` | NEW | `IsGLMVector` / `IsGLMMatrix` template traits + `Normalize` / `Cross` forward decls |
| `luth/source/luth/core/diagnostics/Log.h` | MOVE | from `core/Log.h` |
| `luth/source/luth/core/diagnostics/Log.cpp` | MOVE | from `core/Log.cpp` |
| `luth/source/luth/core/diagnostics/LogFormatters.h` | MOVE+EDIT | from `core/LogFormatters.h`; **add** ostream<< Vec3/Mat4 operators (extracted from `LuthTypes.h`) |
| `luth/source/luth/core/diagnostics/Profiler.h` | MOVE | from `core/Profiler.h` |
| `luth/source/luth/core/time/Time.h` | MOVE | from `core/Time.h` |
| `luth/source/luth/core/time/Timer.h` | MOVE | from `core/Timer.h` |
| `luth/source/luth/core/LuthTypes.h` | DELETE | Replaced by `types/` split |
| `luth/source/luth/core/Math.h` | DELETE | Renamed → `types/LuthMath.h` |
| `luth/source/luthpch.h` | EDIT | Reroute includes to new sub-folder paths |
| (~129 caller files in `luth/source/` + `luthien/source/`) | EDIT | Bulk perl rewrite of `#include "luth/core/X.h"` → `#include "luth/core/<subfolder>/X.h"` |

**Stay in `core/` (top-level lifecycle, not a candidate for relocation):**
- `App.{h,cpp}`, `EntryPoint.h`, `Version.{h,cpp}`, `FrameData.h`
- `UUID.{h,cpp}`, `EditorHooks.{h,cpp}`, `ProjectFile.{h,cpp}`

**Verify:**
- [ ] Clean Debug x64 build (zero errors, no new warnings)
- [ ] Clean Release x64 build (catches missing transitive includes)
- [ ] `rg "luth/core/(LuthTypes|Math|Log|Profiler|Time|Timer|LogFormatters)\\.h"` returns zero hits — all callers route through new paths
- [ ] Editor launches identically — pure file move, no behavior change

---

### C: Wrap-up + v2.3.0 release

**Commit:** `chore(release): core-reorg → v2.3.0`
**Trailer:** `Closes #81`

| File | Change | Notes |
|------|--------|-------|
| `luth/source/luth/core/Version.h` | EDIT | Bump to 2.3.0 |
| `docs/development/ROADMAP.md` | EDIT | Add v2.3.0 row to Completed Epics table; remove `core-reorg` from Planned table |
| `docs/development/ARCHITECTURE.md` | EDIT | Update Core sub-tree showing types/diagnostics/time |
| `CLAUDE.md` | EDIT | Update Source Structure section + bump Current/Next version |
| `docs/development/history/v2.x/core-reorg.md` | NEW | Epic writeup |
| `docs/development/epics/core-reorg.md` | DELETE | This spec — promoted to history |

**After commit C:**
- `git checkout main`
- `git merge --no-ff epic/core-reorg -m "feat(release): merge epic/core-reorg (#81)"`
- `git tag -a v2.3.0 -m "v2.3.0 — core-reorg"`
- `git push origin main --follow-tags`
- `gh release create v2.3.0 --title "v2.3.0 — core-reorg" --notes-file docs/development/history/v2.x/core-reorg.md --latest`

---

## Architecture Notes

### Target layout

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
│   ├── LuthTypes.h          primitives (i8…u64, f32/f64, byte, fs alias)
│   ├── LuthMath.h           Vec/Mat/Quat aliases + Math:: facade + Luth:: helpers (Assimp, AABB, Frustum)
│   └── TypeTraits.h         IsGLMVector/Matrix + Normalize/Cross fwd decls
├── diagnostics/
│   ├── Log.{h,cpp}
│   ├── LogFormatters.h      fmt formatters for STL/Vulkan + ostream<< for Vec3/Mat4
│   └── Profiler.h           Tracy macros
└── time/
    ├── Time.h               static engine clock + dt
    └── Timer.h              scoped stopwatch
```

### Why split LuthTypes.h

Currently `LuthTypes.h` mixes 5 unrelated concerns:
1. Primitive aliases (i8/u32/f32/byte) — universally pulled by every TU
2. GLM type aliases (Vec3/Mat4/Quat) — only needed by math-aware code
3. Type traits (IsGLMVector/Matrix) — meta-programming for math layer
4. Forward decls for Math facade (Normalize/Cross)
5. ostream<< formatters for Vec3/Mat4 — diagnostics concern

Split lets a translation unit pull only what it needs (in theory; in practice the PCH pulls everything, so the split is conceptual + organizational, not compile-time).

### Why merge Math.h into types/LuthMath.h

`Math.h` already contains the Vec/Mat/Quat ALIAS users (in fact, the `Math::Translate` etc. wrappers depend on those aliases). Combining alias defs + facade in one file removes the artificial separation introduced by the math-abstraction epic for Pass A/B clarity. The math-abstraction history explicitly flagged this:

> The two-file split is conceptual (primitives vs operations) and matches the eventual E4 reorg where both move to `core/types/`.

### Why ostream<< moves to LogFormatters

Custom `operator<<` overloads exist solely to enable `LH_CORE_INFO("{}", vec3)` via spdlog/fmt's ostream adapter. They're a logging concern, not a type concern. Moving them lets `LuthMath.h` stay headers-only without `<spdlog/fmt/ostr.h>` baggage.

### What stays in core/ root

`App`, `EntryPoint`, `Version`, `FrameData`, `UUID`, `EditorHooks`, `ProjectFile` are top-level engine lifecycle. They aren't categorically grouped (no two share a domain), so nesting them serves no readability gain. Per the source plan: "stays — top-level lifecycle".

### PCH / build impact

`luthpch.h` includes the relocated headers explicitly. Updating it once propagates new paths to every TU through PCH. Premake uses `source/**.h` recursive globs, so subfolders pick up automatically — no `premake5.lua` change needed. VS Solution Explorer will reflect the folder tree on next regen.

---

## References

- `~/.claude/plans/analyze-my-engine-in-magical-moore.md` — E4 section (lines 143–186)
- `docs/development/history/v2.x/math-abstraction.md` — sibling epic that flagged the eventual E4 merge of Math.h + LuthTypes.h
- Prior art: `arch-cleanup` (v1.6.0) — `events/` extracted from `platform/`, `Components.h` split into `components/`

---

## Progress Tracker

| Sub-Task | Status | Commit | Date |
|----------|--------|--------|------|
| A: Spec scaffold | pending | — | — |
| B: Reorganize core/ subfolders + include rewrite | pending | — | — |
| C: Wrap-up + v2.3.0 release | pending | — | — |
