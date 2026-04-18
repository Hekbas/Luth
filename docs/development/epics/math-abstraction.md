# Epic: Math Abstraction Enforcement

**Issue:** [#80](https://github.com/Hekbas/Luth/issues/80)  |  **Target:** v2.2.0  |  **Est.:** Medium (1 week)  |  **Deps:** none (E1, E2 done)

---

## Goal

Enforce `Vec*/Mat*/Quat` aliases and a `Luth::Math::*` function/constant facade across the engine and editor. After this epic, only `LuthTypes.h` and `Math.h` include `<glm/...>`. Engine code never sees `glm::` directly, never includes `<glm/...>`, and never references `std::numbers`/`std::numeric_limits` for math values.

---

## Sub-Tasks and Commit Plan

### A: Reshape LuthTypes.h primitive layer

**Commit:** `refactor(core): reshape LuthTypes.h primitive layer`
**Trailer:** `Part of #80`
**Issue items:**
- Reshape LuthTypes.h primitive layer — add IVec/UVec/Mat2, drop unused PI/EPSILON/... constants

| File | Change | Notes |
|------|--------|-------|
| `luth/source/luth/core/LuthTypes.h` | EDIT | Add `IVec2/3/4`, `UVec2/3/4`, `Mat2` aliases + sizeof static_asserts. Drop unused `PI/TWO_PI/HALF_PI/EPSILON/FLOAT_MAX/FLOAT_MIN` (zero callers — verified). |

**Verify:**
- [ ] `scripts/build/build_windows.bat` (Debug x64) succeeds
- [ ] No new warnings

---

### B: Build Luth::Math facade

**Commit:** `feat(core): build Luth::Math facade with constants and function wrappers`
**Trailer:** `Part of #80`
**Issue items:**
- Build Luth::Math facade in Math.h — templated constants + 22 function wrappers + ValuePtr/MakeVec3

| File | Change | Notes |
|------|--------|-------|
| `luth/source/luth/core/Math.h` | EDIT | Add `namespace Luth::Math { ... }` with templated constants (Pi/TwoPi/HalfPi/QuarterPi/InvPi/DegToRad/RadToDeg/SmallNumber/KindaSmallNumber/FloatMax/FloatLowest/FloatMin/MachineEpsilon/E/Sqrt2), 22 function wrappers (Translate/Rotate/Scale/Perspective/Ortho/LookAt/Inverse/Transpose/Normalize/Length/Dot/Cross/Mix/Slerp/EulerAngles/Radians/Degrees/Clamp/Min/Max/Abs/ToMat4), `ValuePtr<T>`, `MakeVec3`, `QuatLookAt`, `length_t`/`qualifier` aliases. Add `<glm/gtc/quaternion.hpp>` explicit. Rewrite existing helper signatures (`ComposeTransform`, `Frustum`, `AABB`) to use `Vec3/Mat4` aliases. Replace `Math.h:108-109` direct `numeric_limits<float>::max()` with `Math::FloatMax<f32>`/`Math::FloatLowest<f32>`. |

**Verify:**
- [ ] Debug x64 builds (additive — no callers yet)
- [ ] `Math.h` is the only file (besides `LuthTypes.h`) including `<glm/...>` after this commit lands

---

### C: Pass A (engine) — types

**Commit:** `refactor(luth): migrate engine to Vec*/Mat* aliases (Pass A)`
**Trailer:** `Part of #80`
**Issue items:**
- Pass A (luth/source): bulk-rewrite types glm::vec*/glm::mat* → Vec*/Mat*

Bulk perl in-place across `luth/source/`:
- `glm::vec2/3/4` → `Vec2/3/4`
- `glm::ivec2/3/4` → `IVec2/3/4`
- `glm::uvec2/3/4` → `UVec2/3/4`
- `glm::mat2/3/4` → `Mat2/3/4`
- `glm::quat` → `Quat`

**Exclude `LuthTypes.h`** — its trait specializations must keep `glm::vec`/`glm::mat`.

**Verify:**
- [ ] Engine target (`Luth.lib`) builds
- [ ] Editor target fails at `EditorHooks.h` field-type mismatch — expected; resolved in commit D
- [ ] `rg "glm::(vec|ivec|uvec|mat|quat)\b" luth/source` returns only `LuthTypes.h` matches

---

### D: Pass A (editor) — types

**Commit:** `refactor(luthien): migrate editor to Vec*/Mat* aliases (Pass A)`
**Trailer:** `Part of #80`
**Issue items:**
- Pass A (luthien/source): same for editor

Same patterns as commit C, applied to `luthien/source/`.

**Verify:**
- [ ] Full solution builds Debug + Release x64
- [ ] `rg "glm::(vec|ivec|uvec|mat|quat)\b" luthien/source` returns zero non-comment matches

---

### E: Pass B (engine) — functions

**Commit:** `refactor(luth): route engine math through Luth::Math facade (Pass B)`
**Trailer:** `Part of #80`
**Issue items:**
- Pass B (luth/source): bulk-rewrite functions glm::translate etc. → Math::Translate

Bulk perl in-place across `luth/source/`:
- `glm::translate` → `Math::Translate`
- `glm::rotate` → `Math::Rotate`
- `glm::scale` → `Math::Scale`
- `glm::perspective/ortho/lookAt/inverse/transpose/normalize/length/dot/cross/mix/slerp/eulerAngles/radians/degrees/clamp/min/max/abs/toMat4/mat4_cast/conjugate/quatLookAt` → `Math::Perspective/Ortho/...`
- `glm::value_ptr` → `Math::ValuePtr`
- `glm::make_vec3` → `Math::MakeVec3`
- `glm::two_pi<f32>()` → `Math::TwoPi<f32>`

**Verify:**
- [ ] Debug x64 builds
- [ ] `rg "glm::[a-z]+\(" luth/source` returns only `Math.h` matches

---

### F: Pass B (editor) — functions

**Commit:** `refactor(luthien): route editor math through Luth::Math facade (Pass B)`
**Trailer:** `Part of #80`
**Issue items:**
- Pass B (luthien/source): same for editor

Same patterns as commit E, applied to `luthien/source/`.

**Verify:**
- [ ] Debug x64 builds
- [ ] `rg "glm::[a-z]+\(" luthien/source` returns zero matches

---

### G: Purge direct glm includes

**Commit:** `chore(core): purge direct glm includes engine-wide`
**Trailer:** `Part of #80`
**Issue items:**
- Purge #include <glm/...> from non-facade files

| File | Change | Notes |
|------|--------|-------|
| 35 files in `luth/source/` and `luthien/source/` | EDIT | Remove `#include <glm/...>` lines. Add `#include "luth/core/Math.h"` only where the consumer needs the facade and didn't already get it via PCH. |

**Verify:**
- [ ] Debug + **Release** x64 build (Release is stricter)
- [ ] `rg "<glm/" luth/source luthien/source` returns only `LuthTypes.h` and `Math.h`
- [ ] `rg "glm::" luth/source luthien/source` returns only the trait specializations in `LuthTypes.h` and the facade body in `Math.h`

---

### [USER SMOKE TEST gate]

Pause for runtime visual verification — see Architecture Notes for the full checklist.

---

### H: Wrap-up

**Commit:** `chore(release): math-abstraction → v2.2.0`
**Trailer:** `Closes #80`
**Issue items:**
- Wrap-up: docs + CLAUDE.md convention + version bump v2.2.0 + history file

| File | Change | Notes |
|------|--------|-------|
| `luth/source/luth/core/Version.cpp` | EDIT | Bump to v2.2.0 |
| `CLAUDE.md` | EDIT | Add Math convention to Conventions section |
| `docs/development/ROADMAP.md` | EDIT | Mark E3 complete |
| `docs/development/history/v2.x/math-abstraction.md` | NEW | Mirror sections of `shader-asset-pipeline.md` |
| `docs/development/epics/math-abstraction.md` | DELETE | Spec retired on epic close |

**Verify:**
- [ ] Debug x64 builds
- [ ] `git log --oneline epic/math-abstraction` shows 8 commits

---

## Architecture Notes

### Constants design (Math:: namespace)

```cpp
namespace Luth::Math {
    // Standard constants — delegate to std::numbers
    template<class T> inline constexpr T Pi             = std::numbers::pi_v<T>;
    template<class T> inline constexpr T E              = std::numbers::e_v<T>;
    template<class T> inline constexpr T Sqrt2          = std::numbers::sqrt2_v<T>;

    // Derived / engine-specific
    template<class T> inline constexpr T TwoPi          = T(2) * Pi<T>;
    template<class T> inline constexpr T HalfPi         = Pi<T> / T(2);
    template<class T> inline constexpr T QuarterPi      = Pi<T> / T(4);
    template<class T> inline constexpr T InvPi          = T(1) / Pi<T>;
    template<class T> inline constexpr T DegToRad       = Pi<T> / T(180);
    template<class T> inline constexpr T RadToDeg       = T(180) / Pi<T>;

    // Tolerances
    template<class T> inline constexpr T SmallNumber       = T(1e-8);
    template<class T> inline constexpr T KindaSmallNumber  = T(1e-4);

    // Limit sentinels
    template<class T> inline constexpr T FloatMax          = std::numeric_limits<T>::max();
    template<class T> inline constexpr T FloatLowest       = std::numeric_limits<T>::lowest();
    template<class T> inline constexpr T FloatMin          = std::numeric_limits<T>::min();
    template<class T> inline constexpr T MachineEpsilon    = std::numeric_limits<T>::epsilon();
}
```

### Special-case symbol handling

| GLM symbol | Math wrapper |
|---|---|
| `glm::mat4_cast(quat)` | `Math::ToMat4(const Quat&)` (same as `glm::toMat4`) |
| `glm::value_ptr(v)` | `template<class T> Math::ValuePtr(const T&)` + non-const overload |
| `glm::make_vec3(float*)` | `Math::MakeVec3(const float*)` |
| `glm::length_t`, `glm::qualifier` | `using Math::length_t = glm::length_t;` etc. |
| `glm::two_pi<f32>()` | `Math::TwoPi<f32>` |
| `glm::clamp/min/max/abs` | Function templates (used on both scalars and vectors) |
| `glm::quatLookAt` | `Math::QuatLookAt(const Vec3&, const Vec3&)` |
| `glm::conjugate`, `glm::decompose` | Used only inside `Math.h` — keep private |

### `IsGLMVector`/`IsGLMMatrix` traits

These detect glm types — Pass A's bulk-rewrite must NOT touch them. Strategy: exclude `LuthTypes.h` from Pass A entirely; hand-edit only the alias additions there.

### Bulk-rewrite tooling

Per `~/.claude/projects/.../memory/reference_bulk_rewrite.md`: use **perl in-place** (preserves CRLF on Windows). Word-boundaried regex:
```
perl -pi -e 's/\bglm::vec3\b/Vec3/g' file1 file2 ...
```

After each pass, rebuild and re-grep to catch missed patterns.

### End-to-end smoke test (before commit H)

1. Delete `luth/Library/Artifacts/`
2. Launch `runtime/Luthien.exe`, load default project
3. Render checklist (visually identical to v2.1.0): PBR + CSM shadows + GTAO + bloom + skybox + outline + grid + ImGui
4. Editor camera (orbit/pan/fly/F-focus/Shift+F)
5. Scene Panel gizmos (translate/rotate/scale)
6. Inspector Vec3 sliders + Quat→Euler display
7. Shader hot-reload still fires
8. Frame Debugger captures + replays

---

## References

- Plan: `~/.claude/plans/continue-the-architecture-review-dapper-eclipse.md`
- Master architecture review: `~/.claude/plans/analyze-my-engine-in-magical-moore.md` (E3 section)
- Prior epic for wrap-up structure: `docs/development/history/v2.x/shader-asset-pipeline.md`
- Bulk-rewrite tool reference: memory `reference_bulk_rewrite.md`

---

## Progress Tracker

| Sub-Task | Status | Commit | Date |
|----------|--------|--------|------|
| A: Reshape LuthTypes.h primitive layer | done | eaeaa00 | 2026-04-18 |
| B: Build Luth::Math facade | done | 7d91417 | 2026-04-18 |
| C: Pass A (engine) — types | done | 9c6f2b9 | 2026-04-18 |
| D: Pass A (editor) — types | done | 430beca | 2026-04-18 |
| E: Pass B (engine) — functions | done | fc55bd8 | 2026-04-18 |
| F: Pass B (editor) — functions | done | c9188f3 | 2026-04-18 |
| G: Purge direct glm includes | pending | — | — |
| H: Wrap-up + v2.2.0 | pending | — | — |
