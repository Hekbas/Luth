# Epic: [Name]

**Issue:** #N  |  **Target:** vX.Y.Z  |  **Est.:** [Small/Medium/Large] ([time])  |  **Deps:** [epic-slug, ...]

---

## Goal

[1-3 sentences summarizing the epic objective. Copy from the GitHub issue goal section.]

---

## Sub-Tasks and Commit Plan

<!-- Each sub-task = one commit. Pre-write the commit message. List the exact GitHub issue checkbox items covered. -->

### A: [Sub-task Name]

**Commit:** `type(area): description`
**Trailer:** `Part of #N` (or `Closes #N` for the final sub-task)
**Issue items:**
- [Checkbox text from GitHub issue]
- [...]

| File | Change | Notes |
|------|--------|-------|
| `path/to/file.h` | NEW | [Brief description] |
| `path/to/file.cpp` | EDIT | [What changes] |

**Verify:**
- [ ] Build succeeds (no errors, no new warnings)
- [ ] [Specific visual/behavioral check]
- [ ] No Vulkan validation errors

---

### B: [Next Sub-task]

**Commit:** `type(area): description`
**Trailer:** `Closes #N`
**Issue items:**
- [...]

| File | Change | Notes |
|------|--------|-------|

**Verify:**
- [ ] Build succeeds
- [ ] [...]

---

## Architecture Notes

<!-- Extract only the relevant sections from BACKLOG.md, TECHNICAL_DEEPDIVE.md, and arch/ files.
     Include: key data structures, shader layouts, API patterns, descriptor set changes. -->

---

## References

- `docs/development/BACKLOG.md` — [epic-slug] section
- `docs/development/arch/[relevant-file].md`
- Prior art: `[existing code path]`

---

## Progress Tracker

<!-- Update after each commit. This is the handoff mechanism between conversations. -->

| Sub-Task | Status | Commit | Date |
|----------|--------|--------|------|
| A: [Name] | pending | — | — |
| B: [Name] | pending | — | — |

<!-- Status values: pending | in-progress | done | deferred -->
