# comment-policy — engine-wide comment audit

**Date:** 2026-05-07
**Issue:** none (solo-dev shortcut)
**Branch:** `chore/comment-policy`
**Tag / version:** none — comment-only, no runtime delta, so no `Version.h` bump
**Estimate:** L

---

## Overview

Engine-wide comment audit. Every C++ file under `luth/source/`, `luthien/source/`, `runtime/source/` was visited and reworked against the calibration brief at `docs/development/comments-refactor-notes.md`. No code logic changed, no file moves, no interface changes — purely comment edits, with a couple of small drive-bys (an `ispatched` typo, a stale "via friend" reference left by the subsystem refactor).

The audit applies five categories of edits in parallel:

1. **Drop banned patterns retroactively.** Version refs (`v2.9.0`, `v2.8.x`), `since vX`, `epic:`, sub-task identifiers, old phase numbering (`Phase 14C`, `Phase 7D`, `S9:`) all converted to factual descriptions of behavior. The hook regex returns zero hits across the audited tree post-merge.
2. **Trim restate-the-code.** Thinking-out-loud comments ("Wait, the API says…"), redundant note pairs, and bug-report-style comments deleted in favor of concise contracts.
3. **Compress block separators.** ~50 multi-line `// =====` banners across the engine collapsed to single-line `// ── X ──` headers. Stale banners (e.g., a "Main Update" banner that survived the subsystem refactor sitting above an unrelated `GetNamedTexture`) dropped entirely.
4. **Expand atomic-ordering rationale.** `std::memory_order_relaxed` sites in the job system and `MainThreadPump` annotated with their pairing rationale (advisory stats, queue-mutex-protected counter, V4 wakeup pair). Release-acquire pairings on IO-thread slot reuse documented.
5. **Add cross-file invariants.** Page-swap rationale in `TaggedPageAllocator::Allocate`, PBR-stride mirror dependency in `GeometrySubsystem`, command-pool ownership in `CommandAllocator`, IsRecording / V3 thread-affinity in `CommandAllocatorPool`.

A second pass extended the audit beyond the original cluster plan once a coverage check showed ~65% of files had been left untouched by the heuristic-driven first pass:

6. **Add class-level headers** to every public `.h` that lacked one. Subject-first or verb-first openings, period-terminated sentences, lines wrapping ~120-130 chars. Calibrated against the engine's existing canonical voice in `RenderingSystem.h`, `LightingSubsystem.h`, `JobSystem.h`. Roughly 100 headers added.
7. **Convert `///` Doxygen blocks to `//`** in the 8 files that used the foreign style: `App.h`, `EditorHooks.h`, `ProjectFile.h`, `GPUTimerPool.h`, `AssetDatabase.h`, `FileSystem.h`, `EditorColors.h`, `ProjectLauncher.h`. Reflowed prose along the way.
8. **Restyle existing `Name — role` headers** (the AI-tell pattern) to varied openings. Em-dash diet: kept where it's punctuation (asides, mid-sentence pauses), dropped where it was a colon-substitute on header lines. Line-length recalibration from ~80 → ~120 across all touched files.
9. **Roadmap-tied phase ref triage.** 52 hits → 32 converted to factual descriptions, 20 kept (those refer to local code structure inside a single function — `RenderGraph::Execute`'s two-phase split, `App::App`'s init steps, `AssetDatabase::LoadProject`'s scan/process/cleanup phases, `FrameDebuggerContext`'s replay-then-copy steps).

---

## What was preserved (don't-touch list)

The following CLAUDE.md / brief categories were treated as load-bearing and left intact wherever the comment was already correct:

- **File-top doxygen-style class-purpose comments** that describe class role.
- **V1-V6 hazard refs in `luth/jobs/`** — every `V1`/`V2`/`V3`/`V4`/`V5`/`V6` mention preserved verbatim. The V<n> banner at the top of `JobSystem.cpp` and `JobSystem.h` stays. SpinLock V1 contract intact. `RecordingScope` V3 IsRecording invariant intact.
- **Threading invariants** — `Caller must hold X`, `Render thread only`, memory-order rationale, fence-poll budgets. `WorkStealingDeque`'s Chase-Lev fence comments, `MPMCQueue`'s V4 generation-counter ABA-safe comments, `BoneMatrixBuffer`'s game-stage-only assertions.
- **Algorithm citations** — `CascadeBuilder.cpp` Engel "Practical Split" + Sascha Willems direct-port reference both intact. Anti-shimmer 1/16 quantization rationale intact.
- **`see arch/...md` cross-references** — every existing one preserved; several added (frame-pipeline, memory, fiber-system, scene-ecs, animation-system, editor, rendering-pipeline).
- **`IEditorHooks` nullptr-safe contract** — preserved (per `arch/editor.md`).
- **GTAO + Global subsystems' `invariant:`-marked atomic-write batches** — these multi-line comments are the canonical documentation of the cross-set co-batched UBO write pattern; not trimmed.
- **`AssetSerializer.h` format-version annotations** (`V2+:` on per-field comments) — these track binary-artifact format compatibility, distinct from code-version archeology.

---

## Calibration lessons

Three honest misses worth recording for any future audit:

1. **Heuristic-driven scoping under-counts.** The "files with zero non-banner comments in first 30 lines" filter that drove the header pass missed any file with even one stale comment. Net effect: 65% of files in scope were skipped on the first sweep. Future audits should run `git grep` against the canonical hook regex *and* enumerate files with `// X — role` AI-tell openers, not just files with no comments at all.
2. **Phase 1 inventory under-counts banned hits.** The plan-mode density inventory showed `renderer/backend/vulkan/` with 0 banned hits, but `VulkanBuffer.cpp:53` and `BoneMatrixBuffer.cpp:96` were real hits surfaced only by per-cluster re-grep. `ResourcePanel.h:12` (a one-line snapshot struct comment) was missed by three Phase 1 agents and the Phase 2 plan agent — surfaced only by the final wrap-up grep. Single-line comments embedded in struct definitions don't show up under "ML-block" or "banned-hit hot folders" heuristics.
3. **AI-tell calibration matters mid-pass, not after.** The first ~50 headers I wrote opened with `Name — role` and wrapped at 80 chars — both AI fingerprints, both rejected by the user mid-pass. A separate restyle pass had to repair them. Future audits should sample 5 headers and get a calibration sign-off before sweeping.

---

## Drive-by issues flagged (out of scope)

Flagged but not fixed during the audit; each becomes a separate issue if/when prioritized:

- **`luth/source/luth/jobs/JobSystem.cpp:257`** — the first `s_Data.HighQueue.GetGeneration();` call (no assignment) is a dead no-op. The const accessor has no side effects; only `WakeByAddressSingle` two lines below actually wakes a worker. The misleading "Touch to trigger wake" comment was rewritten during this audit, but the dead call line itself remains untouched (comment-only audit scope precluded the code edit).

---

## Verification

```bash
# Build clean Debug x64
scripts\build\build_windows.bat

# Banned-pattern grep — must return zero hits
git grep -nE '\b(v[0-9]+\.[0-9]+|Pillar [A-Z]|since v[0-9]|as of [0-9A-Z]|epic:)\b' \
    luth/source/ luthien/source/ runtime/source/

# Doxygen check — must return zero
grep -rn "^\\s*///" luth/source/ luthien/source/ runtime/source/

# Roadmap-tied phase refs — must return zero (local Phase 1/2/3/4/5 inside functions are kept)
grep -rnE "^\\s*//.*\\b[Pp]hase 1[3-9]" luth/source/ luthien/source/ runtime/source/
```

Smoke gate is comment-only — no runtime behavior change. Validation is "read the diff and confirm tone."

---

## Files modified summary

- **163 files** modified across `luth/source/`, `luthien/source/`, `runtime/source/`
- **+835 / -537** lines net (incl. this history file)
- No files added beyond the history file, none deleted
- Single commit landed directly on `main` (no branch merge — comment-only audit, no bisect value)
- No tag, no `Version.h` bump — comment-only audits don't carry a runtime version delta

---

## Related notes

- Calibration brief: `docs/development/comments-refactor-notes.md` (untracked, project-local)
- Style enforcement: `scripts/git-hooks/{pre-commit,commit-msg}` (banned-pattern regex, ML-block exemption rule, ≤60-char subject, ≤5-bullet body)
- Cornerstone arch docs the audit aligned against:
  - `docs/development/arch/fiber-system.md` — V1-V6 hazards normative
  - `docs/development/arch/memory.md` — `FreeTag(N-2)` semantics
  - `docs/development/arch/rendering-pipeline.md` — Set ownership, frame-cycling
  - `docs/development/arch/scene-ecs.md` — system iteration order
  - `docs/development/arch/editor.md` — `IEditorHooks` nullptr-safe contract
  - `docs/development/arch/animation-system.md` — bone matrix upload contract
  - `docs/development/arch/frame-pipeline.md` — Game/Render/GPU pipeline shape
