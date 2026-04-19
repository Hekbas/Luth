# Epic: animation-split

**Issue:** [#82](https://github.com/Hekbas/Luth/issues/82)  |  **Target:** v2.4.0  |  **Est.:** Small (< day)  |  **Deps:** —

---

## Goal

Dissolve `luth/animation/` — move each file next to its actual owner. `BoneMatrixBuffer.{h,cpp}` and `Skeleton.h` + `AnimationClip.h` → `renderer/resources/`; `AnimationController.h` → `scene/components/`; folder deleted. Fixes F7 from the post-v2.0 architecture review.

---

## Sub-Tasks and Commit Plan

### A: Move BoneMatrixBuffer → renderer/resources

**Commit:** `refactor(renderer): move BoneMatrixBuffer to renderer/resources`
**Trailer:** `Part of #82`
**Issue items:**
- `BoneMatrixBuffer.{h,cpp}` → `renderer/resources/`

| File | Change | Notes |
|------|--------|-------|
| `luth/source/luth/animation/BoneMatrixBuffer.{h,cpp}` | MOVE | → `luth/source/luth/renderer/resources/BoneMatrixBuffer.{h,cpp}` |
| 12 consumers (`RenderPipeline.cpp`, 9 `renderer/passes/*.cpp`, `scene/systems/{AnimationSystem,RenderingSystem}.cpp`) | EDIT | `luth/animation/BoneMatrixBuffer.h` → `luth/renderer/resources/BoneMatrixBuffer.h` |

**Verify:**
- [ ] Build Debug x64 succeeds, no new warnings
- [ ] `rg "luth/animation/BoneMatrixBuffer"` returns 0 matches

---

### B: Move Skeleton + AnimationClip → renderer/resources

**Commit:** `refactor(renderer): move Skeleton/AnimationClip to renderer/resources`
**Trailer:** `Part of #82`
**Issue items:**
- `Skeleton.h` + `AnimationClip.h` → `renderer/resources/`

| File | Change | Notes |
|------|--------|-------|
| `luth/source/luth/animation/Skeleton.h` | MOVE | → `luth/source/luth/renderer/resources/Skeleton.h` |
| `luth/source/luth/animation/AnimationClip.h` | MOVE | → `luth/source/luth/renderer/resources/AnimationClip.h` |
| `renderer/resources/Model.h`, `renderer/resources/BoneMatrixBuffer.h`, `resources/importers/ModelImporter.{h,cpp}`, `scene/systems/AnimationSystem.h` | EDIT | Update include paths |

**Verify:**
- [ ] Build Debug x64 succeeds
- [ ] `rg "luth/animation/(Skeleton|AnimationClip)\.h"` returns 0 matches

---

### C: Move AnimationController → scene/components + delete folder

**Commit:** `refactor(scene): move AnimationController to scene/components`
**Trailer:** `Part of #82`
**Issue items:**
- `AnimationController.h` → `scene/components/`
- Delete empty folder

| File | Change | Notes |
|------|--------|-------|
| `luth/source/luth/animation/AnimationController.h` | MOVE | → `luth/source/luth/scene/components/AnimationController.h` |
| `scene/components/Animation.h`, `scene/systems/AnimationSystem.h` | EDIT | Update include paths |
| `luth/source/luth/animation/` | DELETE | folder empty after the move |

**Verify:**
- [ ] Build Debug x64 succeeds
- [ ] `ls luth/source/luth/animation/` → folder does not exist
- [ ] `rg "luth/animation/"` in `luth/source` + `luthien/source` returns 0 matches

---

### D: User smoke test

- Skinned mesh renders + animates + casts shadow (`BoneMatrixBuffer` descriptor set still binds)
- Frame Debugger captures + replays a frame (exercises every descriptor binding touched by the move)

---

### E: Docs + v2.4.0 + history

**Commit:** `chore(release): animation-split → v2.4.0`
**Trailer:** `Closes #82`
**Issue items:**
- Docs + v2.4.0 + history

| File | Change | Notes |
|------|--------|-------|
| `docs/development/ARCHITECTURE.md` | EDIT | Remove dedicated `[Animation]` section; update tree pointers for renderer/resources + scene/components |
| `docs/development/arch/animation-system.md` | EDIT | Update paths (`animation/Skeleton.h` → `renderer/resources/Skeleton.h`, etc.) |
| `docs/development/ROADMAP.md` | EDIT | Move `animation-split` row from Planned → Completed |
| `docs/development/history/v2.x/animation-split.md` | NEW | Full wrap-up |
| `luth/source/luth/core/Version.h` | EDIT | `VERSION_MINOR` 3 → 4 |
| `docs/development/epics/animation-split.md` | DELETE | This file — removed when the epic closes |

**Verify:**
- [ ] Build Debug x64 succeeds
- [ ] `GetVersionString()` reads "2.4.0"

---

## Architecture Notes

Current mixed ownership of `luth/animation/` (5 files, 390 LOC):

- **`Skeleton.h` + `AnimationClip.h`** — pure asset data. Stored by value inside [`renderer/resources/Model.h`](luth/source/luth/renderer/resources/Model.h:138). Imported via `ModelImporter`. Belong with `Model` in `renderer/resources/`.
- **`AnimationController.h`** — declared in `namespace Luth::Component` (line 8) yet filed outside `scene/components/`. Referenced only by `scene/components/Animation.h` and `scene/systems/AnimationSystem.h`.
- **`BoneMatrixBuffer.{h,cpp}`** — Vulkan singleton SSBO, descriptor set Set 4 binding 0. `Init`/`Shutdown` called by `RenderPipeline`; `AllocateBlock/FreeBlock/UploadBones` called by `AnimationSystem`; `GetDescriptorSet`/`GetDescriptorSetLayout` consumed by 9 render passes + `RenderPipeline`.

Codebase rule (precedent: `renderer/lighting/`, `renderer/resources/`, `scene/components/`): scene components are pure CPU data, Vulkan lives in `renderer/`, asset data held by a renderer resource lives in `renderer/resources/`. The dissolve realigns `animation/` with this rule.

No new code. Pure relocation. API of `BoneMatrixBuffer` unchanged; struct definitions unchanged; namespaces unchanged (`Luth` for `Skeleton`/`AnimationClip`/`BoneMatrixBuffer`; `Luth::Component` for `AnimationController`).

---

## References

- `docs/development/arch/animation-system.md` — data-model diagrams and pipeline
- `docs/development/history/v2.x/core-reorg.md` — atomic-move + perl-rewrite playbook used here
- Prior art: [`luth/source/luth/renderer/resources/Model.h`](luth/source/luth/renderer/resources/Model.h:138) already holds `Skeleton m_Skeleton` and `std::vector<AnimationClip> m_AnimationClips` by value

---

## Progress Tracker

| Sub-Task | Status | Commit | Date |
|----------|--------|--------|------|
| A: Move BoneMatrixBuffer | pending | — | — |
| B: Move Skeleton + AnimationClip | pending | — | — |
| C: Move AnimationController + delete folder | pending | — | — |
| D: User smoke test | pending | — | — |
| E: Docs + v2.4.0 + history | pending | — | — |
