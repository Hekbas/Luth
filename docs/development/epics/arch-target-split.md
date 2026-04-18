# Epic: arch-target-split

**Issue:** [#78](https://github.com/Hekbas/Luth/issues/78)  |  **Target:** v2.0.0  |  **Est.:** Medium (2–3 days)  |  **Deps:** arch-renderer-split (v1.7.0, ✅)

---

## Goal

Phase 5 of the architecture refactor — the final and highest-risk epic. Extract ~12 071 LOC of editor code out of `Luth.lib` into a dedicated `Luthien.lib`, thin the current `luthien/` exe down to a `runtime/` launcher. After the epic, `Luth.lib` contains zero ImGui / panel / inspector / `EditorStyle` code, the `CLAUDE.md` "Targets" bullet becomes truthful, and Luth can be linked into any future host (standalone player, CI harness, scripted build) without dragging the editor along.

**Deviations from issue #78 (decided during planning):**
- **Sandbox.exe descoped** (was issue sub-task D). Structural guarantee enforced via grep-check instead of a maintained second binary — `Luth.lib`'s file glob excludes `luthien/` physically, and `git grep -l imgui luth/source` gates the invariant. `CLAUDE.md` "Targets" bullet updated to reflect this.
- **Folder layout:** `luthien/source/luthien/` (mirrors `luth/source/luth/`) rather than `editor/source/luthien/`. Keeps the Luthien brand at top level.
- **Sub-task order reversed:** rename exe-folder first (frees the `luthien/` name), then move editor in.

---

## Target Layout

| Target | Kind | Links | Contents |
|--------|------|-------|----------|
| `Luth.lib`     | StaticLib  | —             | Engine only — no ImGui, no panels |
| `Luthien.lib`  | StaticLib  | Luth          | Editor (panels, inspectors, commands, style, widgets) |
| `Luthien.exe`  | ConsoleApp | Luth, Luthien | Editor application (what users run) |

```text
Luth/
├── luth/                   # Luth.lib (engine, unchanged root; editor/ subfolder gone)
│   └── source/luth/
│       ├── core/ jobs/ memory/ platform/ events/ renderer/ resources/ scene/ animation/
│       └── (no editor/)
├── luthien/                # Luthien.lib — NEW home (was Luthien.exe folder)
│   └── source/luthien/
│       ├── Editor.{h,cpp}  EditorCamera.{h,cpp}  EditorSettings.{h,cpp}  EditorStyle.{h,cpp}
│       ├── Command.{h,cpp} CommandHistory.{h,cpp} EditorSelection.h EditorColors.h
│       ├── ProjectLauncher.{h,cpp}  UI.{h,cpp}
│       ├── panels/ inspectors/ commands/ widgets/
│       └── lepch.{h,cpp}   # NEW — editor PCH
├── runtime/                # Luthien.exe — renamed from old luthien/
│   └── source/LuthienApp.cpp  (+ Luthien.rc, icons/Luth.ico, resource.h)
├── extern/ scripts/ docs/ samples/
├── premake5.lua   Luth.sln (regenerated)
```

---

## Sub-Tasks and Commit Plan

### A: Rename `luthien/` → `runtime/`, keep as thin exe linking `Luth`

**Commit:** `refactor(build): rename luthien exe folder to runtime`
**Trailer:** `Part of #78`
**Issue items:**
- Rename `luthien/` → `runtime/`, make it thin (issue sub-task C — brought forward).

| File / Path | Change | Notes |
|---|---|---|
| `luthien/` | MOVE (`git mv`) | → `runtime/`. Preserves `Luthien.rc`, `icons/Luth.ico`, `resource.h`, `source/LuthienApp.cpp`. |
| `runtime/premake5.lua` | EDIT | Rename project `"Luthien"` → `"Runtime"`, `targetname "Luthien"` (output binary stays `Luthien.exe`). Still `kind "ConsoleApp"`, still `links { "Luth", "vulkan-1", "shaderc_shared" }` — Luthien.lib joins in B. |
| `premake5.lua` (top-level) | EDIT | `startproject "Runtime"`; `group "Luthien"` → `include "runtime"` (temporarily — B adds the lib alongside). |
| `runtime/editor_settings.json`, `runtime/imgui.ini` | LEAVE | Deleted in C, not here; keep this commit scoped to the rename. |

**Verify:**
- [ ] `scripts\setup\setup_windows.bat` regenerates cleanly
- [ ] MSBuild Debug x64: `Luth.lib` + `Luthien.exe` build unchanged
- [ ] `Luthien.exe` launches, renders scene, full editor functionality (editor still in `luth/` — we moved nothing semantic)
- [ ] `git log --follow runtime/source/LuthienApp.cpp` shows full pre-rename history

---

### B: Extract editor into `luthien/source/luthien/` as `Luthien.lib`

**Commit:** `refactor(build): extract editor into Luthien.lib`
**Trailer:** `Part of #78`
**Issue items:**
- Create `editor/` top-level project (`Luthien.lib`) — *layout deviation: landed at `luthien/` instead.*
- Promote engine hooks needed by editor (folded in).

| File / Path | Change | Notes |
|---|---|---|
| `luth/source/luth/editor/**` | MOVE (`git mv`) | → `luthien/source/luthien/**`. 30+ files + `panels/` `inspectors/` `commands/` `widgets/` subtrees. |
| `luthien/premake5.lua` | NEW | `project "Luthien"`, `kind "StaticLib"`, `links { "Luth", "imgui", "ImGuizmo" }`, same Vulkan includedirs as `Luth`, PCH `lepch.h`. |
| `luthien/source/luthien/lepch.{h,cpp}` | NEW | Editor PCH — `<imgui.h>`, `<backends/imgui_impl_vulkan.h>`, `<vulkan/vulkan.h>`, editor-common STL. |
| All references | EDIT (perl) | `#include "luth/editor/...` → `#include "luthien/...` repo-wide. ~50 files: runtime/LuthienApp.cpp, panel cross-includes, any engine code that accidentally referenced editor (should be zero — verify). |
| Editor `.cpp` PCH | EDIT (perl) | `#include "luthpch.h"` → `#include "lepch.h"` across all moved editor `.cpp` (~35 files). |
| `premake5.lua` (top-level) | EDIT | `group "Luthien"` now includes both `include "luthien"` and `include "runtime"`. Order: lib before exe. |
| `runtime/premake5.lua` | EDIT | Add `"Luthien"` to `links {}`; add `%{wks.location}/luthien/source` to `includedirs` so `#include "luthien/Editor.h"` resolves. |
| `luth/premake5.lua` | EDIT | No explicit change needed — `source/**` glob stops picking editor after the move. Confirm via regen diff. |

**Engine-hook promotion (folded in — handle iteratively as compile/link surfaces them):**

| Known site | Resolution |
|---|---|
| `Editor.cpp:86` `static_cast<VulkanBackend*>(Renderer::GetBackend())` | No change — both types public. Optional polish: `Renderer::GetVulkanBackend()` accessor (defer to phase 6). |
| `ScenePanel.cpp:293` `std::static_pointer_cast<VKTexture>` | No change — keep cast; full Target abstraction = phase 6. |
| `Editor.cpp:101-108` inline `vkCreateDescriptorPool` | Optional: promote to `VulkanContext::CreateImGuiPool()` helper (clean single-call-site home). Defer unless link-clean requires it. |
| Anonymous-namespace / PCH-private symbols | Promote to public header one-by-one, scope tight (no drive-by API expansion). |

**Contingency:** if too large for one commit, sub-stage:
- **B1:** `git mv` + `luthien/premake5.lua` skeleton + top-level regen. Build broken — acceptable intermediate.
- **B2:** Include + PCH rewrites repo-wide. Compile-clean editor, may still have link errors.
- **B3:** Hook promotions surfaced by linker. Link-clean; feature parity.

**Verify:**
- [ ] `scripts\setup\setup_windows.bat` regenerates 3 projects: `Luth`, `Luthien` (lib), `Runtime` (exe)
- [ ] MSBuild Debug x64: all three build clean, no new warnings
- [ ] `Luthien.exe` launches, loads project, renders scene, all panels functional, hot-reload works
- [ ] `git log --follow luthien/source/luthien/Editor.cpp` shows full pre-move history
- [ ] `git grep -n 'luth/editor/' luth/source` → zero matches (engine contains no editor include path)
- [ ] No Vulkan validation errors in ImGui descriptor path

---

### C: Untrack editor-state files

**Commit:** `chore(repo): untrack editor state files`
**Trailer:** `Part of #78`
**Issue items:**
- Clean up tracked editor state files (issue sub-task E).

| File / Path | Change | Notes |
|---|---|---|
| `runtime/editor_settings.json` | `git rm --cached` | Post-A location. |
| `runtime/imgui.ini` | `git rm --cached` | |
| `luth/editor_settings.json` | `git rm --cached` | Engine folder holding editor state — appeared in `git status`. |
| `samples/cache/pipeline.bin` | `git rm` | Already deleted in working tree (`git status` showed `D`). |
| `.gitignore` | EDIT | Add `editor_settings.json` (catches all locations), `samples/cache/`, keep existing `*imgui.ini`. |

**Verify:**
- [ ] Fresh clone + Debug build + Luthien.exe launch → `git status` clean
- [ ] Editor settings still persist across Luthien.exe restarts (files created untracked in working tree)

---

### D: Premake regen, engine-cleanness grep-verify, docs, version bump, history, release

**Commit:** `chore(build): finalize target split, bump v2.0.0`
**Trailer:** `Closes #78`
**Issue items:**
- Premake regeneration + workspace validation (issue sub-task F).

| File / Path | Change | Notes |
|---|---|---|
| `Luth.sln`, `*.vcxproj`, `*.vcxproj.filters` | REGEN | Final clean regeneration — 3 projects (Luth, Luthien [lib], Runtime [exe → Luthien.exe]). |
| `CLAUDE.md` | EDIT | "Targets" bullet: `Luth.lib` (engine), `Luthien.lib` (editor), `Luthien.exe` (editor app). Remove Sandbox mention. "Source Structure" updated — editor under `luthien/source/luthien/` at repo root, not under `luth/source/luth/editor/`. |
| `luth/source/luth/core/Version.h` | EDIT | Bump to `v2.0.0` (architecture-change versioning rule per `ROADMAP.md`). |
| `docs/development/ROADMAP.md` | EDIT | Move `arch-target-split` from Planned → Completed. |
| `docs/development/history/v1.x/arch-target-split.md` | NEW | History file — overview, sub-task log, directory changes, `Luth.lib` shrinkage delta, key design decisions (Sandbox descoping rationale, layout choice, sub-task reorder), build verification. Style = `arch-renderer-split.md`. |
| `docs/development/epics/arch-target-split.md` | DELETE | Epic spec removed on close per workflow. |

**Engine-cleanness grep verification (part of this commit's verify, captured in history file):**
- `git grep -l 'imgui' luth/source` → zero matches
- `git grep -l 'Luthien\|luthien/' luth/source` → zero matches
- `git grep -l '\bImGui::\|ImGui_' luth/source` → zero matches

**Post-commit (outside the commit itself):**
- `gh release create v2.0.0 --title "v2.0.0 — arch-target-split" --notes-file docs/development/history/v1.x/arch-target-split.md --latest`

**Verify:**
- [ ] Single MSBuild invocation builds all 3 targets clean, no new warnings
- [ ] `Luthien.exe` launches + full editor functionality
- [ ] `Luth.lib` binary size delta captured in history file (`dumpbin /headers bin/Debug/Luth/Luth.lib`)
- [ ] All three grep checks pass (zero matches)
- [ ] GitHub issue #78 auto-closed by `Closes #78` trailer
- [ ] `gh release view v2.0.0` shows release with history file as notes

---

## Architecture Notes

### One-way dependency invariant
`Luth.lib` must never `#include` a `luthien/...` header. D's grep check gates this permanently.

### VS project-name + binary-name split
New `luthien/premake5.lua` owns `project "Luthien"` → outputs `Luthien.lib`. Old `luthien/premake5.lua` owned `project "Luthien"` → outputs `Luthien.exe`. Name collision resolved: exe project (now at `runtime/`) renamed `project "Runtime"` with `targetname "Luthien"` so the output binary stays `Luthien.exe`. `startproject "Runtime"`.

### PCH strategy
`luthpch.h` remains the engine PCH. New `lepch.h` in `luthien/source/luthien/` carries editor-heavy includes (`<imgui.h>`, ImGuizmo headers, STL bits used by panels). B swaps every editor `.cpp`'s `#include "luthpch.h"` → `#include "lepch.h"` via mechanical `perl -i -pe 'BEGIN{binmode(ARGV);binmode(STDOUT);}'` — CRLF-preserving per `arch-cleanup` lesson.

### Why no Sandbox.exe
Earlier experiments had a Sandbox target; it was deleted because it served no perceived purpose and cluttered the solution. The *structural* reason to have one — proving `Luth.lib` ships without editor — is enforced more cheaply: after B, `Luth.lib`'s `files { "source/**" }` glob physically excludes `luthien/`, so editor symbols cannot link in. D's grep verification (`git grep -l imgui luth/source` → zero) turns that guarantee into a CI-visible invariant without a maintained binary. A future epic can re-introduce a player / standalone harness when there's a real consumer.

### Why not introduce a Target/Viewport abstraction now
`ScenePanel` / `FrameDebuggerPanel` currently downcast `Texture` → `VKTexture` and call `ImGui_ImplVulkan_AddTexture`. A clean abstraction is an `ImTextureID ToImGui(Texture*)` backend hook or a full `RenderTarget` type. **Out of scope** — mixing a structural split with an API redesign triples the risk. Phase 6 work (post-v2.0.0) introduces an RHI / presentation layer and drops the `VKTexture` cast then. Editor `#include`s of `VulkanTexture.h` / `VulkanContext.h` remain through this epic.

### Shrinkage target
`Luth.lib` today bundles:
- 30+ editor `.cpp` (panels, inspectors, commands, style, widgets, `Editor.cpp`, `UI.cpp`)
- FontAwesome defs (~1 424 LOC in `editor/widgets/Icons.h`)
- ImGui Vulkan backend binding glue

Expected post-epic: `Luth.lib` object size ↓. Exact delta measured in D, captured in history file.

---

## References

- Issue: [#78](https://github.com/Hekbas/Luth/issues/78)
- Multi-epic plan: [`docs/development/ARCH-REFACTOR-PLAN.md`](../ARCH-REFACTOR-PLAN.md)
- Prior art: [`docs/development/history/v1.x/arch-cleanup.md`](../history/v1.x/arch-cleanup.md), [`docs/development/history/v1.x/arch-renderer-split.md`](../history/v1.x/arch-renderer-split.md)
- Template: [`docs/development/epics/TEMPLATE.md`](./TEMPLATE.md)

---

## Progress Tracker

| Sub-Task | Status | Commit | Date |
|----------|--------|--------|------|
| A: Rename `luthien/` → `runtime/`                             | pending | — | — |
| B: Extract editor → `luthien/source/luthien/` (`Luthien.lib`) | pending | — | — |
| C: Untrack editor-state files                                 | pending | — | — |
| D: Regen + grep-verify + docs + v2.0.0 + history + release    | pending | — | — |
