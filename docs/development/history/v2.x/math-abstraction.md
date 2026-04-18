# v2.2.0 — math-abstraction

**Date:** 2026-04-18
**Commits:** 8 (on `epic/math-abstraction`)
**Issue:** [#80](https://github.com/Hekbas/Luth/issues/80)

---

## Overview

Second epic of the post-v2.0 architecture-review series. Built **`Luth::Math`** as a single-source facade for every glm type and function the engine uses. After this epic, only `LuthTypes.h` and `Math.h` include `<glm/...>`; engine and editor code use `Vec*/Mat*/Quat` aliases and `Math::*` wrappers exclusively.

Minor version bump to **v2.2.0** per the ROADMAP MINOR rule (one completed epic with engineering-visible changes — single math include, modern C++20 templated constants, type-aware tolerances, latent `numeric_limits::min` vs `lowest` confusion fixed in AABB).

---

## Sub-Tasks

| # | Sub-task | Commit |
|---|---|---|
| — | Spec scaffold | `8ed0300 docs(epic): add math-abstraction spec` |
| A | Reshape LuthTypes.h primitive layer | `eaeaa00 refactor(core): reshape LuthTypes.h primitive layer` |
| B | Build Luth::Math facade | `7d91417 feat(core): build Luth::Math facade with constants and function wrappers` |
| C | Pass A engine — types | `9c6f2b9 refactor(luth): migrate engine to Vec*/Mat* aliases (Pass A)` |
| D | Pass A editor — types | `430beca refactor(luthien): migrate editor to Vec*/Mat* aliases (Pass A)` |
| E | Pass B engine — functions | `fc55bd8 refactor(luth): route engine math through Luth::Math facade (Pass B)` |
| F | Pass B editor — functions | `c9188f3 refactor(luthien): route editor math through Luth::Math facade (Pass B)` |
| G | Purge direct glm includes | `1bef56b chore(core): purge direct glm includes engine-wide` |
| H | Docs + v2.2.0 + history + wrap-up | `chore(release): math-abstraction → v2.2.0` |

---

## Type Aliases (LuthTypes.h)

### Before

```cpp
using Vec2 = glm::vec2;  using Vec3 = glm::vec3;  using Vec4 = glm::vec4;
using Mat3 = glm::mat3;  using Mat4 = glm::mat4;
using Quat = glm::quat;

constexpr f32 PI       = 3.14159265358979323846f;
constexpr f32 TWO_PI   = 2.0f * PI;
constexpr f32 HALF_PI  = 0.5f * PI;
constexpr f32 EPSILON  = std::numeric_limits<f32>::epsilon();
constexpr f32 FLOAT_MAX = std::numeric_limits<f32>::max();
constexpr f32 FLOAT_MIN = std::numeric_limits<f32>::min();   // smallest *positive*, almost never what callers want
```

### After

```cpp
using Vec2 = glm::vec2;  using Vec3 = glm::vec3;  using Vec4 = glm::vec4;
using IVec2 = glm::ivec2; using IVec3 = glm::ivec3; using IVec4 = glm::ivec4;
using UVec2 = glm::uvec2; using UVec3 = glm::uvec3; using UVec4 = glm::uvec4;
using Mat2 = glm::mat2;  using Mat3 = glm::mat3;  using Mat4 = glm::mat4;
using Quat = glm::quat;
// Constants block deleted — moved to Luth::Math (templated)
```

The 6 root-namespace constants had **zero call sites** when verified — dead code. Reborn under `Math::` as templated `inline constexpr` variables. The `EPSILON = numeric_limits::epsilon()` was a particularly misleading name (machine epsilon ≈ 1.19e-7 vs. spatial tolerance ~1e-4). Replaced by named tolerances (`SmallNumber`, `KindaSmallNumber`) and explicit `MachineEpsilon` for the rare ULP-level case.

---

## Math Facade (Math.h)

### Constants

```cpp
namespace Luth::Math {
    // Standard — delegate to <numbers>
    template<class T> inline constexpr T Pi    = std::numbers::pi_v<T>;
    template<class T> inline constexpr T E     = std::numbers::e_v<T>;
    template<class T> inline constexpr T Sqrt2 = std::numbers::sqrt2_v<T>;

    // Derived
    template<class T> inline constexpr T TwoPi     = T(2) * Pi<T>;
    template<class T> inline constexpr T HalfPi    = Pi<T> / T(2);
    template<class T> inline constexpr T QuarterPi = Pi<T> / T(4);
    template<class T> inline constexpr T InvPi     = T(1) / Pi<T>;
    template<class T> inline constexpr T DegToRad  = Pi<T> / T(180);
    template<class T> inline constexpr T RadToDeg  = T(180) / Pi<T>;

    // Tolerances (engine value-add)
    template<class T> inline constexpr T SmallNumber      = T(1e-8);
    template<class T> inline constexpr T KindaSmallNumber = T(1e-4);

    // Sentinels — delegate to <limits>
    template<class T> inline constexpr T FloatMax       = std::numeric_limits<T>::max();
    template<class T> inline constexpr T FloatLowest    = std::numeric_limits<T>::lowest();
    template<class T> inline constexpr T FloatMin       = std::numeric_limits<T>::min();
    template<class T> inline constexpr T MachineEpsilon = std::numeric_limits<T>::epsilon();
}
```

### Function wrappers

25 wrappers covering every `glm::*` function the engine uses:

| Group | Functions |
|---|---|
| Transformation | `Translate`, `Rotate`, `Scale`, `Perspective`, `Ortho` (×2), `LookAt` |
| Linear algebra | `Inverse`, `Transpose`, `Normalize`, `Length`, `Length2`, `Dot`, `Cross` |
| Interpolation | `Mix`, `Slerp` |
| Quaternion | `ToMat4` (covers both `glm::toMat4` and `glm::mat4_cast`), `EulerAngles`, `QuatLookAt`, `Decompose` |
| Angle conversion | `Radians`, `Degrees` |
| Common math | `Clamp`, `Min`, `Max`, `Abs` (function templates — work on scalar + vector) |
| Pointer helpers | `ValuePtr` (const + non-const), `MakeVec3` |
| Type re-exports | `length_t`, `qualifier` |

### AABB sentinel fix

```cpp
// Before
struct AABB {
    Vec3 Min = Vec3( std::numeric_limits<float>::max());
    Vec3 Max = Vec3(-std::numeric_limits<float>::max());   // works but not by name
    ...
};

// After
struct AABB {
    Vec3 Min = Vec3(Math::FloatMax<f32>);
    Vec3 Max = Vec3(Math::FloatLowest<f32>);   // explicit lowest()
    void Expand(const Vec3& p) { Min = Math::Min(Min, p); Max = Math::Max(Max, p); }
    ...
};
```

Both old and new code work, but the new spelling makes the intent (`lowest()`, not `min()`) impossible to misread.

---

## Migration

### Pass A — types (commits C, D)

`glm::vec2/3/4`, `glm::ivec2/3/4`, `glm::uvec2/3/4`, `glm::mat2/3/4`, `glm::quat` → `Vec*/IVec*/UVec*/Mat*/Quat` aliases. Word-boundaried perl in-place across **35 files** (28 engine + 7 editor); `LuthTypes.h` excluded so the `IsGLMVector<glm::vec<L,T,Q>>` and `IsGLMMatrix<glm::mat<C,R,T,Q>>` trait specializations stay valid (their job *is* to detect glm types).

### Pass B — functions (commits E, F)

`glm::translate/rotate/scale/...` → `Math::Translate/Rotate/Scale/...`. Same files, same tool. Special cases:
- `glm::two_pi<f32>()` → `Math::TwoPi<f32>` (variable template, no parens)
- `glm::mat4_cast(quat)` and `glm::toMat4(quat)` → unified as `Math::ToMat4`
- `glm::length_t`, `glm::qualifier` → `Math::length_t`, `Math::qualifier` (template-parameter usages in `EditorCamera`)
- `glm::length2` (unscoped from initial inventory) → `Math::Length2` — surfaced during commit E, facade extended in same commit
- `glm::decompose` (full TRS+skew+perspective unpack used by `FrameDebuggerPanel`) → `Math::Decompose` — facade extended in commit E for the editor consumer

### Purge — includes (commit G)

`#include <glm/...>` removed from all 33 non-facade files. `Math.h` and `LuthTypes.h` are now the only files including glm headers. PCH carries the facade into every translation unit transparently — no consumer needs an explicit `Math.h` include.

LuthTypes.h `operator<<` formatter signatures cleaned up: `const glm::vec3&` → `const Vec3&` (typedef-equivalent, just clearer style).

---

## Final Tally

| Metric | Before | After | Delta |
|---|---:|---:|---:|
| Files with `glm::` references | 37 | 2 (`LuthTypes.h`, `Math.h` — the facade) | −35 |
| Total `glm::` references | ~450 | 50 (all inside the facade) | −400 |
| Files with `<glm/...>` includes | 35 | 2 (the facade) | −33 |
| Total `<glm/...>` include lines | ~38 | 10 (facade only) | −28 |

---

## Key Design Decisions

### Single math file vs split

Aliases stay in `LuthTypes.h`; functions and constants live in `Math.h`. PCH (`luthpch.h`) already pulls both, so callers get the full math layer transparently. The two-file split is conceptual (primitives vs operations) and matches the eventual E4 reorg where both move to `core/types/`. Doing it this way keeps E3 a clean enforcement pass — no folder moves, no file renames; E4 will move both files in one shot.

### Templated `inline constexpr` over function form

Modern C++20 idiom (`std::numbers::pi_v<T>` is a variable template). Same expressiveness as the function form (`glm::pi<T>()`) without the trailing parens noise. `Math::Pi<f32>` reads cleanly at the call site and is `consteval`-equivalent for compile-time use.

### Engine value-add: named tolerances

`Math::SmallNumber<T>` (1e-8) and `Math::KindaSmallNumber<T>` (1e-4) borrow Unreal's distinction. Spatial code rarely wants `numeric_limits::epsilon()` (~1.19e-7); it wants a robust tolerance for "essentially zero" or "vectors are equal enough." Naming the two cases makes intent explicit and lets future code change the values without a hunt-and-replace.

### Header-only facade

All wrappers are `inline` one-line forwards to `glm::*`. No `Math.cpp` for E3. The compiler inlines them away (PCH-cached `inline` calls don't show up in the binary). E4 may revisit if folder relocation warrants `.cpp` files for non-inline helpers (`ComposeTransform`, `Frustum`).

### Bulk-rewrite split per source tree

Pass A and Pass B each split into separate luth/luthien commits (rather than one combined Pass A or Pass B). Lets `git bisect` isolate which side of the engine→editor boundary introduced any regression. Adds one extra build per pass — cheap insurance.

### Trait specializations stay glm-typed

`IsGLMVector<glm::vec<L,T,Q>>` and `IsGLMMatrix<glm::mat<C,R,T,Q>>` in `LuthTypes.h` keep `glm::vec`/`glm::mat` because *detecting glm types is their purpose*. Renaming the specialization body would break the abstraction at its foundation. Pass A explicitly excluded `LuthTypes.h`.

---

## Build Verification

- 8 commits on `epic/math-abstraction`; every commit builds Debug x64 clean (zero errors; only pre-existing warnings — C4267/C4244/C4996/LNK4006).
- Release x64 verified at commits D and G (commit G is the strictest gate — surfaces missing transitive includes after the `<glm/...>` purge).
- `rg "glm::" luth/source luthien/source` — only matches inside `LuthTypes.h` (trait specs + alias defs) and `Math.h` (facade body).
- `rg "<glm/" luth/source luthien/source` — only matches in `LuthTypes.h` and `Math.h`.
- `rg "glm::" luthien/source` — zero.

---

## Runtime Verification (user smoke test)

- Delete `luth/Library/Artifacts/` → relaunch: shaders compile, no errors. (Cross-checks E1 still works.)
- Full render: PBR + cascaded shadows + GTAO + bloom + skybox + IBL + outline + grid + ImGui visually identical to v2.1.0.
- Editor camera: orbit / pan / fly / F-focus / Shift+F entity tracking.
- Scene Panel gizmos: translate / rotate / scale all update transforms correctly.
- Inspector: Vec3 sliders (Position, Scale), Quat→Euler display reads correctly.
- Shader hot-reload: edit any `.frag` → live update fires.
- Frame Debugger: capture + replay works.

---

## Lessons

**Inventory accuracy matters more than inventory size.** The pre-execution grep counted 41 unique `glm::` symbols. Two were missed: `glm::length2` (used once by `AnimationSystem` for root motion) and the assumption that `glm::decompose` was only used inside `Math.h` (it's also called directly by `FrameDebuggerPanel`). Both surfaced during the migration passes and required facade extensions mid-epic. The save: each pass commit re-grepped for survivors before declaring done — caught both within the same commit, no stale facade left dangling.

**`numeric_limits::min` is a footgun for floats.** It returns the *smallest positive normal* (~1.18e-38), not the most negative finite value (`numeric_limits::lowest()` does that). The old `FLOAT_MIN = numeric_limits::min()` constant would have silently miscompared had any caller picked it up. Naming the two values explicitly (`Math::FloatMin` vs `Math::FloatLowest`) makes the choice impossible to fumble.

**Typedef-aliased boundaries don't break.** The plan flagged `EditorHooks.h` as a high-risk file because changing `glm::vec3` fields to `Vec3` crosses the engine→editor ABI. In practice, the typedef makes them binary-identical: editor code that still spelled `glm::vec3` reads engine-side `Vec3` fields via identity copy. Pass A's "engine-only" commit built the editor clean — the risk evaporated. Smaller risks usually do.

**Bulk-rewrite + per-pass build is cheap insurance.** Pass A and Pass B each split into engine + editor commits with builds in between. Total cost: ~30 s of extra builds per epic. Total benefit: clean `git bisect` lanes if anything visual regresses, and confidence that each pass compiles standalone before stacking the next.
