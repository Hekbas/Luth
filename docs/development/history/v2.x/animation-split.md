# v2.4.0 — animation-split

**Date:** 2026-04-19
**Commits:** 4 (on `epic/animation-split`)
**Issue:** [#82](https://github.com/Hekbas/Luth/issues/82)

---

## Overview

Fourth epic of the post-v2.0 architecture-review series. `luth/animation/` — a catch-all folder that had collected five files serving three distinct owners since v1.7 `arch-renderer-split` — dissolved. Each file lands next to its actual owner:

- **Asset data** (`Skeleton.h`, `AnimationClip.h`) → `renderer/resources/`, next to `Model.h` which already held them by value.
- **GPU SSBO singleton** (`BoneMatrixBuffer.{h,cpp}`) → `renderer/resources/`, next to other Vulkan resource wrappers.
- **Scene component** (`AnimationController.h`) → `scene/components/`, aligning the path with its existing `namespace Luth::Component` declaration.

No runtime behavior change. Pure relocation: unchanged APIs, unchanged struct definitions, unchanged namespaces. Minor version bump to **v2.4.0** per the ROADMAP rule ("each completed epic with engineering-visible changes"), same precedent as v2.3.0 `core-reorg`.

---

## Sub-Tasks

| # | Sub-task | Commit |
|---|---|---|
| A | Spec scaffold | `ae0ff47 docs(epic): add animation-split spec` |
| B | Move BoneMatrixBuffer → renderer/resources | `2f0ecf1 refactor(renderer): move BoneMatrixBuffer to resources` |
| C | Move Skeleton + AnimationClip → renderer/resources | `7855d87 refactor(renderer): move Skeleton/AnimationClip to resources` |
| D | Move AnimationController → scene/components + delete folder | `2a00f82 refactor(scene): move AnimationController to scene/components` |
| E | Docs + v2.4.0 + history + wrap-up | `chore(release): animation-split → v2.4.0` |

Three code commits split by destination folder — each independently buildable, each a clean `git mv` + perl-sweep atom.

---

## Why a full dissolve, not just `BoneMatrixBuffer`

The original E5 scope was a "tiny epic" that moved only `BoneMatrixBuffer` out of `luth/animation/` and left the three data headers in place. On review, that split fixed only one of three mis-placements:

1. `BoneMatrixBuffer.{h,cpp}` — a Vulkan singleton with no business in a non-renderer folder.
2. `Skeleton.h` + `AnimationClip.h` — pure asset data, imported via `ModelImporter`, stored by value inside `renderer/resources/Model.h`. A `Model.h` reaching across into `luth/animation/` for its own members is a dependency inversion.
3. `AnimationController.h` — declared `namespace Luth::Component` but filed outside `scene/components/`. The path disagreed with the namespace.

Leaving the three data headers in place would have preserved a three-file, 160-LOC folder with no interdependent code — not a domain, just a leftover grouping. Dissolve picks the stricter interpretation: every file sits with the code that actually owns it.

A "scene-nest" alternative (`luth/scene/animation/` keeping all three data headers together) was rejected because it would force `renderer/resources/Model.h` to include from `luth/scene/animation/`, contradicting the rule `renderer/ includes nothing from scene/` enforced elsewhere.

---

## Target layout

```
luth/source/luth/
├── renderer/resources/
│   ├── BoneMatrixBuffer.{h,cpp}   ← from animation/  (Vulkan SSBO, Set 4)
│   ├── Skeleton.h                 ← from animation/  (asset data, held by Model)
│   ├── AnimationClip.h            ← from animation/  (asset data, held by Model)
│   └── Model.h, Mesh.h, Texture.h, Buffer.h (existing)
└── scene/
    ├── components/
    │   ├── Animation.h             (existing)
    │   └── AnimationController.h   ← from animation/  (namespace Luth::Component)
    └── systems/
        └── AnimationSystem.{h,cpp} (existing, include paths updated)
```

`luth/source/luth/animation/` is gone.

---

## Migration

### File moves (`git mv` — rename detection preserves per-file history)

| From | To |
|---|---|
| `luth/animation/BoneMatrixBuffer.h` | `luth/renderer/resources/BoneMatrixBuffer.h` |
| `luth/animation/BoneMatrixBuffer.cpp` | `luth/renderer/resources/BoneMatrixBuffer.cpp` |
| `luth/animation/Skeleton.h` | `luth/renderer/resources/Skeleton.h` |
| `luth/animation/AnimationClip.h` | `luth/renderer/resources/AnimationClip.h` |
| `luth/animation/AnimationController.h` | `luth/scene/components/AnimationController.h` |

### Include rewrite (`perl -pi -e`, preserving CRLF)

One pass per moved header. 23 consumer include sites touched total:

| Moved header | Consumers |
|---|---|
| `BoneMatrixBuffer.h` | `RenderPipeline.cpp`, 9 `renderer/passes/*.cpp`, `AnimationSystem.cpp`, `RenderingSystem.cpp` — 12 sites |
| `Skeleton.h` | `Model.h`, `BoneMatrixBuffer.h`, `ModelImporter.{h,cpp}`, `AnimationSystem.h` — 5 sites |
| `AnimationClip.h` | `Model.h`, `ModelImporter.{h,cpp}`, `AnimationSystem.h` — 4 sites |
| `AnimationController.h` | `scene/components/Animation.h`, `AnimationSystem.h` — 2 sites |

Each commit is a self-contained atom (move + rewrite + build), so `git bisect` can land on any of them and produce a working binary.

### Build system

Premake `files { "source/**.h", "source/**.cpp" }` — recursive glob picks up the new paths automatically. No `premake5.lua` edit needed. PCH (`luth/source/luthpch.h`) didn't reference any of the moved headers; no PCH change.

---

## Final tally

| Metric | Before | After |
|---|---:|---:|
| Files in `luth/animation/` | 5 | 0 (folder deleted) |
| `#include "luth/animation/..."` references | 23 | 0 |
| Refactor commits | — | 3 (each builds Debug x64 clean) |

---

## Key Design Decisions

### Three refactor commits, one per destination folder

`core-reorg` (v2.3.0) bundled its moves into one atomic commit because the PCH rewire would straddle half-moved state if split. `animation-split` has no such constraint — the moves target three independent destinations and the consumer lists are disjoint. Splitting by destination gives a clean narrative in `git log` and surgical blame: each commit answers a single "why did this include change?" question.

### No new abstractions

No new class, no new header, no wrapper. `Model.h` already held `Skeleton m_Skeleton` and `std::vector<AnimationClip> m_AnimationClips` by value. Moving the headers next to their consumer rewards the existing contract rather than inventing one.

---

## Build Verification

- 3 refactor commits on `epic/animation-split`; every commit builds Debug x64 clean.
- Only pre-existing warnings present (LNK4006 from vulkan-1.lib, C4996 `getenv`/`strncpy` in editor, C4244 chrono narrowing in `Editor.cpp:420`).
- `rg "luth/animation/"` returns 0 matches across `luth/source` and `luthien/source`.
- `ls luth/source/luth/animation/` → folder does not exist.
- Runtime smoke (user-tested): skinned mesh renders + animates + casts shadow; Frame Debugger captures + replays a frame.

---

## Lessons

**"Move only the obvious piece" was the wrong scope.** The plan identified F7 ("two different owners") correctly, then proposed to move only the one piece that clearly belonged elsewhere. That left two more mis-placements unaddressed and preserved a folder whose only remaining justification was its name. When a plan calls for a full fix but proposes a partial one, re-examine the plan.

**Namespace-vs-path mismatches are a reliable smell.** `AnimationController` sat in `namespace Luth::Component` while filed outside `scene/components/`. That pairing is almost always a later refactor that moved adjacent code without moving this one along. `rg "namespace Luth::Component"` against the source tree is a cheap first pass for future architecture reviews.
