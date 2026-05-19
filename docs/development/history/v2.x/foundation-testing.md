# v2.11.1 — foundation-testing

**Date:** 2026-05-19
**Commits:** 8 (on `feat/foundation-testing`)
**Issue:** [#124](https://github.com/Hekbas/Luth/issues/124)

---

## Overview

Stress-test harness for the foundational systems that bite hardest when they
break — JobSystem, sync primitives, allocators, lock-free queues. Motivated
by the v2.10.0 `WaitForCounter` UAF that cost ~15 hours of trap-based
debugging; a 200-LOC stress test under ASan would have caught it in seconds.
This series stands up that test infrastructure: `LuthTests.exe` console binary
with doctest, a `DebugASan` build configuration, fixtures, and an initial
suite covering the V1–V6 hazard catalog from
[`arch/fiber-system.md`](../arch/fiber-system.md).

Final state: **28 test cases, 199 assertions, full pass under `DebugASan` in
~5 seconds.** Both smoke (every-build) and stress (nightly / on-push) tiers.
The flagship V1b regression test (stack-local Counter + lemming pattern)
exists and validates that custom fibers + ASan correctly track per-fiber
stacks across cross-worker DecrementCounter races.

The series shipped in two halves, with [`v2.11.0` — custom-fibers](custom-fibers.md)
as a prerequisite. Win32 fibers couldn't satisfy ASan's per-fiber stack
tracking (bounds opaque until after first SwitchToFiber); custom MASM context
switching backed by `VirtualAlloc` stacks unlocked ASan, which then enabled
this stress harness to function at all. Foundation-testing's first attempt
(originally tagged then force-push-undone) discovered the fiber problem;
custom-fibers fixed it; foundation-testing resumed on rebased main and is
this tag.

Two bugs caught along the way:

1. **`LinearAllocator::GetTotalSize` undercounted oversized pages.** The
   accounting multiplied page count by `m_DefaultPageSize`, missing the
   custom-size pages created when `Allocate(size > defaultPage)` triggers
   the oversize path. Fixed to sum `m_PageSizes` directly. Caught by
   `test_linearallocator`'s oversized-request case.
2. **Tracy's static-init dbghelp thread tripped ASan strlen interceptor.**
   Tracy spawns a `SymbolWorker` during `s_profiler`'s dynamic initializer;
   the worker calls dbghelp.dll which internally reads past a stack buffer.
   ASan's `strlen` interceptor flags it (~20% flake on V1b stress). Fix:
   disable `TRACY_ENABLE` under `DebugASan` (we're sanitizing here, not
   profiling — Tracy.lib becomes effectively empty). Tracy unaffected in
   Debug/Release/Dist.

The V1b regression check itself is intentional partial-proof: a local
`git revert 17cb1e3` rebuilds Luth.lib with the WaitForCounter UAF
reintroduced; the V1b stress detects the regression as either ASan
`stack-use-after-scope` (when the lemming's fiber stack is still mapped) or
a process segfault (when the fiber's stack has been recycled by the time
the trailing `fetch_sub` lands). Detection is probabilistic — the bug is
famously timing-sensitive — but either failure mode is unambiguous as a
test failure. Post-fix code: 10/10 clean.

GPU heap (`GPUTaggedPageAllocator`) stress was deferred. Setting up a
minimal Vulkan instance + device for a test binary adds ~100 LOC of
boilerplate; the CPU side's V6 overflow test covers the same hazard shape
with the same allocator architecture. Reserved as a follow-up if a real
GPU-side allocator bug surfaces.

---

## Sub-Tasks

| # | What landed | Commit |
|---|---|---|
| A | **Test scaffold.** `LuthTests` console-exe target linking `Luth.lib`. doctest 2.5.2 vendored at `tests/extern/doctest/doctest.h` (single header, version-pinned). `DebugASan` premake configuration (`/fsanitize=address` + Release CRT, MSVC-mandated; workspace-wide `_DISABLE_VECTOR/STRING_ANNOTATION` to unify container-annotation symbols across mixed ASan/non-ASan static libs). `tests/main.cpp` with explicit doctest entry (calls `Luth::Log::Init()` before `doctest::Context::run`). `tests/support/JobSystemFixture.h` (RAII Init/Shutdown per TEST_CASE_FIXTURE — JobSystem static state is not idempotent). `scripts/test/test_windows.bat` runner that auto-locates `clang_rt.asan_dynamic-x86_64.dll` via vswhere. One smoke test exercising the runner. | [`3d8517e`](../../../../commit/3d8517e) |
| B1 | **Tracy DebugASan fix.** Drop `TRACY_ENABLE`/`TRACY_FIBERS`/`TRACY_ON_DEMAND` defines from all DebugASan filters. Tracy's `s_profiler` dynamic-init thread initializes dbghelp.dll, which trips ASan's `strlen` interceptor (~20% flake on V1b stress at 100K iter). Tracy.lib compiles effectively empty under ASan; Debug/Release/Dist profiling is unaffected. | [`592e72c`](../../../../commit/592e72c) |
| B2 | **V1b WaitForCounter UAF regression.** `tests/jobs/test_waitcounter.cpp` — the Naughty-Dog lemming pattern at smoke (10K) and stress (100K) iterations. Stack-local Counter inside the lemming's scope; cross-worker timing (8 leaves on N workers, wake from one, return on another) is what makes the race statistical. Custom fibers (v2.11.0) keep ASan's stack tracking accurate across switches; 10/10 stress runs clean post-fix. Canonical regression check (`git revert 17cb1e3` → bug reintroduced → V1b detects via ASan or segfault → revert the revert) verified by hand. | [`40cdb97`](../../../../commit/40cdb97) |
| B3 | **V2–V5 + AtomicCounter coverage.** Five test files cataloguing the rest of the V-hazards: `test_v2_main_isolation` (assert main thread idx 0 never executes worker jobs, via `GetWorkerThreadId()`), `test_v3_isrecording` (RecordingScope predicate state transitions; can't catch the engine's `assert(!IsRecording)` abort from doctest, so we verify the state machine the assert gates on), `test_v4_wakeup` (200 dispatch+wait cycles with 50μs idle gaps — any lost wakeup blocks the next WaitForCounter indefinitely), `test_v5_inline` (1000-deep recursive Dispatch+WaitForCounter — without `MAX_INLINE_DEPTH=4`, the OS stack would overflow around 50K levels; with V5 the depth is bounded by fiber switches), `test_atomiccounter` (default Value, explicit-init shift, Increment(N) + N Decrements balance, 8-thread concurrent Inc/Dec lands at 0). | [`da3adca`](../../../../commit/da3adca) |
| C1 | **LinearAllocator::GetTotalSize fix.** Previously `page_count * m_DefaultPageSize`, which undercounted custom-size pages created for oversized allocations. Now sums `m_PageSizes` directly. Found by the oversized-request test. | [`aecb31c`](../../../../commit/aecb31c) |
| C2 | **Allocator stress.** `test_linearallocator` (5 cases: 8-aligned default, custom alignment, oversized request + memset validation, page-growth on overflow, Reset rewinds without freeing). `test_taggedpage` (4 cases: aligned Allocate, FreeTag pool recycle, V6 overflow at 128 MB no-FreeTag, 8-thread concurrent allocation across distinct tags). GPU heap deferred; CPU side covers the same V6 architectural shape. | [`b83db00`](../../../../commit/b83db00) |
| D | **Lock-free primitive stress.** `test_spinlock` (basic TryLock state + 8-thread × 50K critical-section increment must preserve count). `test_mpmcqueue` (single-thread FIFO + full-queue back-pressure + Frostbite-style 4P×4C × 25K property test asserting both count and arithmetic sum match — every value popped exactly once, no losses, no duplicates). `test_workstealing` (owner LIFO pop, resize beyond initial capacity, owner-pop + 4-thief steal race where every push must be observed exactly once via `unordered_set` reconciliation across all hauls). | [`31e0c6a`](../../../../commit/31e0c6a) |
| E | **Wrap-up.** `Version.h` patch bump to v2.11.1. This history file. | this commit |

---

## Architectural decisions

### Single tag for the series, no per-effort intermediate tags

The original v2.11.0 attempt force-push-undone earlier in the series was a
revealing experience — per-effort versioning of internal scaffolding turned
out to be over-fragmented (each "sub-task" was 2 commits + a wrap-up, with
little independent shipping value). Foundation-testing's actual deliverable
is the harness as a whole, not the individual sub-tasks. Single tag at
series end mirrors what Godot/Unreal/Bevy do for non-feature work.

### V1b regression: probabilistic detection, both ASan and segfault count

The original v2.10.0 bug was famously hard to reproduce — sporadic stack
corruption after ~15 minutes of physics simulation. Trying to make the
regression test 100% deterministic would require a controlled timing
harness (deliberately delay DecrementCounter's trailing fetch_sub past a
known window). The simpler approach: run the lemming pattern at scale
(100K iter under DebugASan) and detect the bug via EITHER ASan's
stack-use-after-scope (when the fiber's stack is still mapped) OR a
process segfault (when the fiber has been recycled). Both indicate "bug
fired"; both fail the test. 10/10 stress runs clean post-fix.

### V3 RecordingScope: test the predicate, not the abort

`Fiber::SwitchTo` calls `assert(!ctx->IsRecording)` in debug builds. doctest
can't catch `assert()` from userspace — it's an `abort()` call, not a C++
throw. The V3 test instead verifies the STATE MACHINE the assert gates on:
RecordingScope's constructor sets `ctx->IsRecording = true`; destructor
clears it. If the state machine breaks, the assert won't fire under any
test conditions either. We test what we can test.

### V5 InlineDepth: test the consequence, not add a getter

`JobContext::InlineDepth` is internal; exposing it via a getter for tests
would violate the no-test-only-API cornerstone. Instead `test_v5_inline`
exercises the BEHAVIORAL CONSEQUENCE: a recursive Dispatch+WaitForCounter
chain of 1000 levels must complete. Without `MAX_INLINE_DEPTH=4`, each
inline level adds a C++ stack frame and the OS stack overflows at ~50K
levels (each Dispatch+WaitForCounter context is ~1KB). With the depth
limit, every 4 levels triggers a fiber switch which resets stack depth,
so 1000 logical levels run on bounded physical stacks.

### V2 main-thread isolation: assert by tid recording

Main thread is worker index 0 by convention. `JobSystem::GetWorkerThreadId()`
returns it for any thread that's been registered. The V2 test dispatches
10K jobs, each records its worker index into a per-job slot; main spin-
waits on the counter from the main thread (the V2-isolated path). After
completion, no slot should hold 0 — if main had stolen, some slot would
have recorded its tid.

### Tracy disabled under DebugASan, not extern-instrumented

Two options on the table when the dbghelp false positive surfaced: instrument
Tracy.lib with `/fsanitize=address` too (so its internal `strlen` calls go
through ASan-aware paths), or disable Tracy under DebugASan entirely. We
chose to disable. Profiling under ASan is a niche need; the dbghelp init
issue is a Windows system library interacting with ASan's interceptor, not
a fixable code-review-style issue. Tracy.lib gets compiled empty under
DebugASan via missing TRACY_ENABLE; the cost is no fiber-zone visibility
during ASan runs, which is acceptable since we're sanitizing, not profiling.

### GPU heap deferred, not skipped

`GPUTaggedPageAllocator` shares architecture with the CPU TPA — same V6
overflow shape, same tag-bulk-free model. A test for it needs a minimal
`VkInstance` + physical device + logical device + VulkanAllocator + heap
Init — ~100 LOC of boilerplate that doesn't exercise anything new
architecturally. Deferred as a follow-up. If a real GPU-heap bug ever
surfaces, the test pattern is clear from `test_taggedpage`'s V6 case.

### Two engine bug fixes shipped inline, not separately

`LinearAllocator::GetTotalSize` and the Tracy/dbghelp fix landed as part of
the series rather than as standalone hotfixes. Both were discovered BY the
new tests, which is exactly the kind of value the foundation-testing series
exists to provide. Bundling them into the series tag (v2.11.1) keeps the
story coherent: "here's the harness, here's two bugs it caught the day it
shipped."

---

## Files touched

**New (tests)**
- `tests/main.cpp`
- `tests/support/JobSystemFixture.h`
- `tests/support/StressHelpers.h`
- `tests/premake5.lua`
- `tests/extern/doctest/doctest.h` (vendored)
- `tests/jobs/test_smoke.cpp`
- `tests/jobs/test_waitcounter.cpp`
- `tests/jobs/test_atomiccounter.cpp`
- `tests/jobs/test_v2_main_isolation.cpp`
- `tests/jobs/test_v3_isrecording.cpp`
- `tests/jobs/test_v4_wakeup.cpp`
- `tests/jobs/test_v5_inline.cpp`
- `tests/jobs/test_spinlock.cpp`
- `tests/jobs/test_mpmcqueue.cpp`
- `tests/jobs/test_workstealing.cpp`
- `tests/memory/test_linearallocator.cpp`
- `tests/memory/test_taggedpage.cpp`
- `scripts/test/test_windows.bat`
- `docs/development/history/v2.x/foundation-testing.md`

**Modified (engine)**
- `luth/source/luth/memory/LinearAllocator.h` — `GetTotalSize` sums actual page sizes
- `luth/source/luth/core/Version.h` — 2.11.0 → 2.11.1

**Modified (build)**
- `premake5.lua` — DebugASan workspace configuration + Tests group + container-annotation filter
- `luth/premake5.lua` — DebugASan filter (TRACY defines removed)
- `luth/extern/premake5-tracy.lua` — DebugASan filter (no TRACY_ENABLE)
- `luth/extern/premake5-jolt.lua` — DebugASan filter (Release CRT, no ASan)
- `samples/physics_smoke/premake5.lua` — DebugASan filter (TRACY defines removed)
- `tests/premake5.lua` — DebugASan filter (TRACY defines removed)

---

## Verification

- All four configs (Debug / DebugASan / Release / Dist) build clean from a
  fresh `setup_windows.bat` run.
- **Full suite under DebugASan: 28/28 test cases, 199/199 assertions, ~5s.**
- Smoke tier alone (`--test-case-exclude="*stress*"`): 19/19 in ~3s.
- Stress tier alone (`--test-case="*stress*"`): 9/9 in ~2s.
- V1b regression check (manual): `git revert 17cb1e3 && msbuild ...
  -p:Configuration=DebugASan && LuthTests.exe --test-case="*V1b*stress*"`
  produces either ASan stack-use-after-scope or segfault (exit 139) on at
  least one of ~5 runs — bug detected. `git revert` of the revert restores
  the fix; post-fix runs 10/10 clean.
- JobSysProof Debug 600 frames clean (regression-free at the JobSystem
  surface).
- JobSysProof Release 600 frames clean.
- JobSysProof DebugASan 60 frames clean (no ASan reports — engine clean
  under sustained Jolt physics).
