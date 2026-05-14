# jolt-physics — jobsystem-waitforcounter-uaf

**Date:** 2026-05-14
**Series:** `jolt-physics` (v2.10.0, Mode A series-coalesced — no Version.h bump for this effort)
**Commits:** 1 (on `feat/jolt-rigid-bodies`)
**Issue:** _surfaced during the rigid-bodies sub-effort; no standalone issue filed_

---

## Overview

`Luth::JobSystem::WaitForCounter`'s yield path returned to its caller on the wake-up signal without re-checking the counter's Value. Because `DecrementCounter` issues its wake **before** the final `fetch_sub(1)` that clears the busy bit, a resumed waiter could pop its stack frame while the waking `DecrementCounter` was still in-flight. The trailing `fetch_sub(1)` then landed on whatever now occupied the freed slot — typically another fiber's `PhysicsSystem::Update` frame — writing `0xFFFFFFFF` (the 32-bit two's-complement wrap of `0 - 1`).

The fix is `return;` → fall-through-loop at `JobSystem.cpp:778`: after `SwitchTo` returns, the while-loop body finishes naturally and re-checks Value from the top. The busy-bit spin path (lines 728-732) blocks until `fetch_sub(1)` completes; only then does WaitForCounter return.

Caller's local `AtomicCounter` now stays live until **all** DecrementCounter writes complete. Net code delta: 8 lines in `JobSystem.cpp` (one statement removed, two-line invariant comment added) plus a minor `PhysicsSystem.cpp` cleanup that removes a noisy accumulator-clamp warning and adds an explicit `<cstdint>` include.

No tag for this effort — Mode A intermediate, rolls into the eventual `v2.10.0 jolt-physics` series wrap-up.

---

## Investigation

The bug surfaced as a Release-only `/GS` canary corruption inside `PhysicsSystem::Update`'s frame, with a 32-bit `0xFFFFFFFF` wild write at a fiber-stack-relative offset. Reproduces on the 100-body `drop_test.luth` scene with mean-time-to-fire around 2 minutes against an unmodified `Update`, ~16-35 minutes once an 8 KB stack-pad shifts the canary out of the write target zone.

Initial pattern-matching on the value `0xFFFFFFFF` was misleading. It matches every Jolt invalid-handle sentinel — `JPH::BodyID::cInvalidBodyID`, `SubShapeID::cInvalidSubShapeID`, `QuadTree::cInvalidBodyLocation`, `QuadTree::cInvalidNodeIndex`, `FixedSizeFreeList::cInvalidObjectIndex` — and the natural reading is "Jolt is writing one of those through an uninitialized stack pointer." That framing turned out to be a coincidence: any 32-bit `fetch_sub(1)` operating on memory containing zero produces `0xFFFFFFFF`. The pattern told a story about the value's content, not its origin.

Several hypotheses were eliminated up-front. The `LuthJobSystemForJolt` adapter was ruled out by substituting `JPH::JobSystemSingleThreaded` — the bug still reproduced. `TempAllocatorImpl` was ruled out by substituting `TempAllocatorMalloc` — same result. Layer filters are read-only switch-case code. Component fields all have default-member-initializers. The fiber stack is 2 MB and the error code is `0xc0000409` (canary failure), not `0xc00000fd` (stack overflow). ABI/define match between `Luth.lib` and `Jolt.lib` was verified file-by-file (instruction-set defines, `JPH_DEBUG_RENDERER`, `JPH_ENABLE_ASSERTS`, `JPH_OBJECT_STREAM`, default object-layer bits, runtime library, optimization level all aligned). Jolt's built-in `JPH_VERSION_ID` runtime check in `RegisterTypesInternal` would have raised a Trace on mismatch; no Trace fired.

### Trap mechanisms

Two independent wild-write trap mechanisms were built and self-test-verified working:

1. **DR0 hardware watchpoint.** Per-thread DR0/DR7 programmed via `Get/SetThreadContext`; vectored exception handler caught `STATUS_SINGLE_STEP` and logged RIP + symbolised stack via `Luth::StackTrace::Capture`. Self-test deliberately wrote to the watched address from the calling thread; the handler fired and logged within microseconds. Required **Run mode (Ctrl+F5)**, not Debug mode (Alt+F5) — Rider's debugger silently swallowed first-chance `STATUS_SINGLE_STEP` in Debug mode, hiding all fires from the VEH. Worker-thread coverage extended via `Toolhelp32`-based all-thread enumeration plus `SuspendThread`/`SetThreadContext`/`ResumeThread`. Even with all 41 process threads covered, the actual wild write never fired DR0 — strong signal that the writing thread wasn't a thread we'd armed, or the write didn't go through normal CPU memory protection.

2. **`VirtualProtect` page guard.** Marked the 4 KB page containing `&gapSlots[2]` as `PAGE_READONLY` for the lifetime of `Update`; vectored handler caught `STATUS_ACCESS_VIOLATION`, logged the offender, and used the standard unprotect → trap-flag single-step → re-protect dance to let the writing instruction complete. `VirtualQuery` readback confirmed the kernel actually applied `PAGE_READONLY` (`actualProtect=0x2`). Self-test fired the trap correctly on a deliberate write. But on real wild-write events the sentinel diagnostic still fired (proving the corruption happened) while the page guard remained silent — pointing at either kernel-mode writeback semantics or some Windows-specific bypass.

Two independently verified-working traps both failing on the same event was the meta-signal: the model of "what kind of write this is" was wrong. The misdirection was complete — the wild write wasn't a write at all in the form being trapped for.

### Breaking the misdirection

The discriminating experiment was swapping `LuthJobSystemForJolt` for Jolt's stock `JPH::JobSystemThreadPool` (real OS threads, no Luth fibers, no Luth scheduler involvement in Jolt's work). Two outcomes were predicted: if the bug reproduced, it lived inside Jolt's per-pair physics code (or wherever Jolt-controlled threads execute); if it disappeared, it lived in the Luth adapter or scheduler.

The bug reproduced — but the page guard **finally fired**, this stack:

```
[pagewatch] WILD WRITE #2 target=0x7c1b0ffc50 RIP=0x7ff7632a8f2f TID=37048
  #02 Luth::JobSystem::DecrementCounter (JobSystem.cpp:275)
  #03 Luth::JobSystem::FiberEntryPoint  (JobSystem.cpp:331)
```

The corrupting instruction was `counter->Value.fetch_sub(1)` at `JobSystem.cpp:275` — inside the Luth scheduler, not Jolt. The `JobSystemThreadPool` swap was the right discriminator not because it removed Luth's involvement entirely (game/render stages still run on Luth fibers), but because it routed Jolt's work onto Jolt-managed threads where the page guard could observe a writer it hadn't classified as "expected Luth scheduler activity" and so logged it properly.

The `0xFFFFFFFF` was just `0 - 1` wrapped.

### Root cause

In `DecrementCounter`'s `if (old == 2)` block (the "last decrement, count went from 1 to 0" branch):

1. CAS `Value 2 → 1` (busy bit set, count==0).
2. Acquire `counter->Lock`.
3. Read & null `counter->WaitingListHead`.
4. Release `counter->Lock`.
5. Walk waiter list, adding each fiber to `s_Data.ReadyFibers`, calling `WakeByAddressSingle`.
6. `counter->Value.fetch_sub(1)` — clears busy bit, Value goes `1 → 0`.

Step 5 makes a waiter ready to be scheduled. Another worker can pick up that waiter fiber and resume it immediately, before step 6 runs. The resumed fiber re-enters `WaitForCounter` right after `Fiber::SwitchTo` — and the pre-fix code did `return;` here without re-checking Value. The caller's stack frame, containing the counter, pops. Step 6 then executes `fetch_sub(1)` on the now-freed memory. If that freed slot has been reclaimed for another fiber's local data — typically a slot in another running `Update`'s frame, since fiber stacks get reused in predictable patterns — the decrement writes `0xFFFFFFFF` into it.

### Sites affected

Every system that follows the "stack-local `AtomicCounter` + `Dispatch`/`Execute` + `WaitForCounter`" pattern:

- `luth/source/luth/scene/systems/TransformSystem.h:97` — `TransformUpdate` (per-frame, every level of the hierarchy)
- `luth/source/luth/scene/systems/AnimationSystem.cpp:223` — `AnimEval`
- `luthien/source/luthien/Editor.cpp:465` — `Gather` (panel state collection)
- Any future site using the same pattern

The bug surfaced loudest under heavy `PhysicsSystem` load because the physics fiber's frame happens to have a stack-slot at a layout offset that coincidentally aliases recently-popped counters from the same fiber-stack range. It could just as easily corrupt other frames; the 100-body drop scene just made the rate observable.

---

## Verification

Two-stage verification:

1. **With diagnostics active** (sentinel buffer + page guard + DR0 trap + 8 KB padding still in place): 2 hours of continuous Play on `drop_test.luth` (100 dynamic spheres + 1 static plane), no events. The prior mean-time-to-fire under this configuration was 16-35 minutes. Probability of zero events in 120 minutes at a 30-minute mean: `exp(-4) ≈ 1.8%`.

2. **After diagnostic cleanup** (sentinel buffer removed — back to the original 2-minute-crash baseline): 30 minutes of continuous Play, no crash. Under the pre-padding regime this would have crashed roughly 15 times. Zero crashes confirms the fix covers the actual mechanism, not just the diagnostic-padded layout.

Combined with the mechanistic story (exact line identified, exact race understood, fix targeted at the precise failure mode), the patch is solid.

---

## Sub-tasks

| # | Slug | Notes |
|---|---|---|
| A | WaitForCounter yield-path fix | `return;` removed at `JobSystem.cpp:778`; two-line `invariant:` comment added explaining the wake-vs-fetch_sub ordering and why looping back is correct |
| B | PhysicsSystem.cpp residual cleanup | Removed the `LH_CORE_WARN` accumulator-clamp message inside the maxAccum branch (steady-state noise during heavy load); added an explicit `#include <cstdint>` for `uint64_t` usage |

---

## Foundation-testing implications

The bug is the archetypal case for stress-testing foundational primitives:

- **Race condition** between two threads (waker on one worker, resumed waiter on another).
- **Lifetime bug** crossing async boundaries (counter outlives its declared scope by microseconds due to interleaved DecrementCounter ordering).
- **Pattern-based** — would have triggered on any stack-local `AtomicCounter` + `WaitForCounter` callsite, not just the one that surfaced it.
- **Layout-sensitive symptom** — the actual corruption manifested only when the freed slot's address aliased something the rest of the program checked, making the symptom location uncorrelated with the bug location.

A small stress test under ASan or TSan would have caught this in seconds:

```cpp
TEST(JobSystem, WaitForCounter_NoUseAfterStackFree) {
    for (int iter = 0; iter < 100'000; ++iter) {
        std::atomic<int> sentinel{0};
        {
            JobSystem::Counter counter;
            JobSystem::Dispatch(32, 1, [](auto a) {
                static_cast<std::atomic<int>*>(a.data)->fetch_add(1);
            }, &sentinel, &counter, "stress");
            JobSystem::WaitForCounter(&counter);
        }   // ASan/TSan flags any access to counter after this point
        ASSERT_EQ(sentinel.load(), 32);
    }
}
```

A dedicated `foundation-testing` series is queued for after `jolt-physics` ships, with stress-test infrastructure covering `JobSystem`, `AtomicCounter`, `MPMCQueue`, `WorkStealingDeque`, `SpinLock`, `TaggedPageAllocator`, and `GPUTaggedPageAllocator` as priority targets.

---

## Out of scope

- The `JPH_DEBUG_RENDERER` ODR trap (PhysicsSystem.h:93-95 has a conditional member field; `Luthien.lib`/`Runtime.exe` don't define the macro). Confirmed dormant — no transitive includes of `PhysicsSystem.h` from either consumer. Defer until any future commit attempts to add such an include, at which point the build will need `JPH_DEBUG_RENDERER` + `JPH_ENABLE_ASSERTS` added to both `luthien/premake5.lua` and `runtime/premake5.lua` Release filters.
- `Luth::DecomposeTransform` doesn't check `glm::decompose`'s return value (LuthMath.h:242-252). Latent on the kinematic-body path through `PhysicsSystem::SyncTransformsToBodies`. Independent fix.
- Auditing other `WaitForCounter`-like primitives (`YieldFiber`, custom barriers) for similar wake-before-fetch_sub ordering. Reviewed inline as part of the fix; no other sites identified. Re-audit on any new sync primitive.
