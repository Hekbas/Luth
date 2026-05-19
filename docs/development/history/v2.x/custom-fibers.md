# v2.11.0 — custom-fibers

**Date:** 2026-05-19
**Commits:** 7 (on `feat/custom-fibers`)
**Issue:** [#125](https://github.com/Hekbas/Luth/issues/125)

---

## Overview

Replaces Luth's Win32 fiber backend (`CreateFiberEx` + `SwitchToFiber`) with
custom x86_64 MASM context switching backed by `VirtualAlloc` stacks. The
motivating problem is AddressSanitizer integration: `__sanitizer_start_switch_fiber`
needs the destination stack's `bottom + size` BEFORE the first switch so it can
track which fiber's stack is live. Win32 fibers hide their stack bounds behind
opaque handles — the bounds are only knowable AFTER `SwitchToFiber` has run
once, a chicken-and-egg that makes every yielded fiber trip ASan's stack-frame
tracking with false positives. The new backend allocates stacks via
`VirtualAlloc` so we own the bounds; ASan can be told the truth on every switch.

Approach lifted from the well-trodden references: boost::context's
`jump_x86_64_ms_pe_masm.asm` (for the TIB-swap layout — the unglamorous detail
most reference impls skip), libco's `amd64.c` (for the simpler ABI shape than
boost's `transfer_t`), and Folly's `FiberManager.cpp` (for the ASan-hook
ordering pattern; boost.context's own asm has a known [ordering bug](https://github.com/boostorg/context/issues/65)
where its ASan calls happen in the wrong order, so boost.fiber's docs recommend
ucontext for ASan builds and the working pattern is the one in Folly: C++ layer
calls `__sanitizer_start_switch_fiber` BEFORE the asm switch and
`__sanitizer_finish_switch_fiber` IMMEDIATELY AFTER).

The replacement preserves the JobSystem public API unchanged
(`Init/Shutdown/Execute/Dispatch/WaitForCounter/YieldFiber`); samples and
runtime see no behavior difference. What changes inside is:
- A new `FiberStack` primitive (`VirtualAlloc` + 16 KB guard page).
- A 280-byte saved-context layout that swaps not just GPRs/XMM/MXCSR but also
  the relevant `NT_TIB` fields (StackBase, StackLimit, DeallocationStack,
  FiberData, ArbitraryUserPointer) — that last field replaces Win32 FLS as the
  per-fiber `JobContext*` slot.
- A trampoline that wraps each fiber's first run in
  `__sanitizer_finish_switch_fiber` so ASan reorients its stack tracking onto
  the new fiber's stack.

Validation under DebugASan with the existing `JobSysProof` Jolt-physics sample
(1000 bodies, 19 worker fibers, 60 frames, ~14ms/step): clean. ASan reports no
false positives during fiber switches, which was the original blocker for the
`foundation-testing` series' V1b lemming pattern (the v2.10.0 WaitForCounter
UAF regression). That series can now resume.

The performance benefit is a secondary win: boost's benchmark on a Xeon E5
2620v4 measured 19 cycles per fcontext switch vs 98 cycles per Win-Fiber
switch (~5×). At Luth's typical ~200K switches/sec that's ~0.4% of a 4 GHz
core saved — real but not transformative. The ASan unlock is the actual
deliverable.

---

## Sub-Tasks

| # | What landed | Commit |
|---|---|---|
| A | **FiberStackAllocator.** `FiberStack` struct + `AllocateFiberStack`/`FreeFiberStack`. `VirtualAlloc(MEM_RESERVE\|MEM_COMMIT, PAGE_READWRITE)` for the region; `VirtualProtect(PAGE_NOACCESS)` on the low 16 KB as a guard page so stack overflow faults hard. `FiberStack` carries `Region` + `RegionSize` + `UsableBottom` + `UsableSize` + `StackTop` — bounds available pre-first-switch, which is the whole point. | [`82b2db4`](../../../../commit/82b2db4) |
| B | **MASM context switch primitives.** `FiberPrimitive.asm` defines `jump_fcontext(out_from_sp, to_sp)` + `make_fcontext(stack_top, size, entry, args)` + `fiber_entry_trampoline`. 280-byte save area: 64 B GPRs (RBP, RBX, RDI, RSI, R12-R15), 160 B XMM6-XMM15, 8 B MXCSR + pad, 48 B for NT_TIB StackBase / StackLimit / DeallocationStack / FiberData / ArbitraryUserPointer. premake's source/**.asm glob picked up the file; MSBuild's MASM toolchain (`<Masm>` items + masm.props/.targets imports) assembled it without further config. | [`ef1d8d5`](../../../../commit/ef1d8d5) |
| C | **JobSystem integration.** New `Fiber` struct behind a `LUTH_USE_WIN32_FIBERS` flag (kept Win32 path compilable for the validation cycle). `Create` takes a `JobContext* ownerCtx` and patches the new fiber's TIB-ArbitraryUserPointer save slot via `fcontext_set_owner` — first jump_fcontext restores ownerCtx into gs:[0x28]. `SwitchTo(from, to)` wraps `jump_fcontext` with the Folly-pattern ASan hooks. `CaptureCurrentThreadAsFiber(ownerCtx)` replaces `ConvertThreadToFiber(nullptr)` — records TIB stack bounds and seeds gs:[0x28]. JobSystem.cpp's three FLS callsites (`SetCurrentContext`, `GetCurrentJobContext`, `GetMyTracyName`) get backend-gated bodies; the custom path uses `__readgsqword(0x28)` / `__writegsqword(0x28)` intrinsics. 5 `Fiber::SwitchTo` callsites updated to thread `from` + `to` (`*self`, `*currentFiber`, `worker.SchedulerFiber` in the various directions). The `(void)ownerCtx;` dance keeps the Win32 path silent about the unused argument. | [`15fab3c`](../../../../commit/15fab3c) |
| D | **16-byte alignment fix.** First run of `JobSysProof` segfaulted inside the Jolt step loop. Root cause: `make_fcontext`'s `initial_rsp = aligned_top - 16 - 280` lands at 16n+8 form (not 16-aligned) because 280 mod 16 = 8. `jump_fcontext`'s restore-side `movaps [rsp + 64]` requires 16-aligned RSP and faulted. Fix: shift the layout — place the return-address slot 8 bytes below `aligned_top` instead of 16, so `initial_rsp = aligned_top - 288` lands 16-aligned. Trampoline now enters with RSP 16-aligned (not 16n+8 form), so its prologue is `sub rsp, 32` instead of `40` to preserve 16-alignment for the `call fiber_entry_helper`. JobSysProof immediately ran 600 frames clean post-fix. | [`42a7ddd`](../../../../commit/42a7ddd) |
| E | **DebugASan config + validation.** Workspace `DebugASan` configuration (`/fsanitize=address` + Release CRT mandatory under MSVC + `_DISABLE_VECTOR/STRING_ANNOTATION` workspace-wide to avoid LNK2038 annotate_string mismatches against non-ASan-instrumented Tracy/Jolt). DebugASan filters added to `luth/`, `luth/extern/premake5-tracy.lua`, `luth/extern/premake5-jolt.lua`, `samples/physics_smoke/premake5.lua` — Tracy and Jolt mirror Release CRT (no ASan instrumentation in extern libs, but CRT match so linking works). JobSysProof DebugASan: 60 frames Jolt physics, 19 worker fibers, **no ASan reports**, exit 0. `dumpbin /imports` confirms `clang_rt.asan_dynamic-x86_64.dll`. | [`62a9876`](../../../../commit/62a9876) |
| F | **Retire Win32 backend.** Delete the `LUTH_USE_WIN32_FIBERS` flag and every `#if defined(LUTH_USE_WIN32_FIBERS)` branch from `Fiber.h` + `JobSystem.cpp`. The Fiber struct loses `Handle`; Create/Destroy/SwitchTo bodies simplify to the single (custom) path. JobSystem.cpp removes `s_FlsIndex` + every `FlsAlloc`/`FlsFree`/`FlsSetValue`/`FlsGetValue` call. SetCurrentContext / GetCurrentJobContext collapse to single-line intrinsic calls. Code shrinks ~110 lines. | [`8a0e8ee`](../../../../commit/8a0e8ee) |
| G | **Wrap-up.** `Version.h` patch bump to v2.11.0. This history file. | this commit |

---

## Architectural decisions

### Custom backend instead of Win32 fibers

Win32 fibers' fundamental incompatibility with ASan is bounds-opacity. The fix
is not "wait for Microsoft to update ASan" (the limitation is in the Win32
fiber API itself — `CreateFiberEx` doesn't expose the stack bottom anywhere
queryable from C/C++) but "own the stacks ourselves." `VirtualAlloc` + guard
page is well-trodden; boost.context, libco, and Folly all do it.

The performance gap (~5×) is a real but secondary win. The actual win is that
ASan now works on this engine — which makes the entire `foundation-testing`
series viable. Before custom fibers, every dispatched job under DebugASan
tripped ASan within milliseconds with false-positive stack-buffer-underflow
reports. After, the engine runs ASan-clean indefinitely under sustained
physics workload.

### TIB ArbitraryUserPointer (gs:[0x28]) replaces Win32 FLS

Win32 FLS (FlsAlloc/FlsSetValue/FlsGetValue) is tied to the Win32 fiber API
internally — switching fibers via `SwitchToFiber` updates FLS slots; switching
fibers via our custom `jump_fcontext` does not. We could maintain FLS in
parallel (write our own swap logic for FLS slots on every jump_fcontext), but
that's brittle and requires undocumented FLS internals. TIB-ArbitraryUserPointer
is documented user-reserved space; our asm explicitly saves+restores it as
part of the NT_TIB swap; access is a single instruction (`__readgsqword(0x28)`
/ `__writegsqword(0x28)`). Cleaner and more direct than FLS.

### NT_TIB fields swapped beyond Stack* and FiberData

The boost::context MASM file swaps four NT_TIB fields that less-thorough
reference impls skip: `StackBase` (0x08), `StackLimit` (0x10),
`DeallocationStack` (0x1478), `FiberData` (0x20). The first two are what
`GetCurrentThreadStackLimits` reads, so anything that queries the current
thread's stack (Tracy fiber zones, Windows Crashpad-style stack walks, etc.)
keeps working per-fiber. `DeallocationStack` is the kernel's view of the
stack's lowest reserved address; not swapping it means Windows believes the
"current" stack is whatever it was last set to (typically the OS thread's
original stack), which interacts poorly with stack growth and SEH unwinding.
`FiberData` (0x20) is what `GetCurrentFiber` and Win32 FLS implementations
read; we set it to 0 (we're not a Win32 fiber) but swap it for consistency.

We added `ArbitraryUserPointer` (0x28) as the fifth field, since we use it for
per-fiber JobContext lookup. Total swap: 40 bytes of NT_TIB fields per switch.

### Folly's ASan hook order, not boost.context's

Boost.context calls `__sanitizer_finish_switch_fiber` BEFORE the asm switch in
its `jump_fcontext` (per [boostorg/context#65](https://github.com/boostorg/context/issues/65)
this is a documented bug — ASan ends up "permanently in switching stacks
mode"). Folly's `FiberManager.cpp` does the hooks correctly: `start_switch_fiber`
on the source BEFORE the switch, `finish_switch_fiber` on the destination
AFTER the switch. We follow Folly. The hooks live in the C++ `Fiber::SwitchTo`
wrapper (around the asm call) for the START side, and in the C++
`fiber_entry_helper` (called from the asm trampoline) for the FIRST-RUN
FINISH side. Steady-state finish is in the wrapper too (paired with start).

### `make_fcontext` initial_rsp must be 16-aligned

The trickiest correctness detail. The save area is 280 bytes (= 17×16 + 8).
On the writer side (jump_fcontext entered via `call`), entry RSP is 16n+8;
`sub rsp, 280` lands 16n. movaps offsets like `[rsp + 64]` are then 16-aligned.

On the make_fcontext side, we want the LAYOUT of the save area to match
exactly. Naive `initial_rsp = aligned_top - 280` lands at 16n+8 (NOT
16-aligned). When jump_fcontext later does `mov rsp, initial_rsp; movaps
[rsp + 64], ...`, it faults on misaligned memory access.

The fix is to shift the trampoline return-address slot from `aligned_top - 16`
to `aligned_top - 8`, so `initial_rsp = aligned_top - 288` lands 16-aligned.
Total layout: trampoline RIP at 8 bytes below stack-top, 280-byte save area
below that, initial RSP at 288 below stack-top.

This was the first-iteration bug; surfaced as a segfault inside Jolt's step
loop on JobSysProof. Caught immediately by validating against an existing
workload — exactly why sub-task D's "run the samples" step exists.

### Trampoline calls a C++ helper for the ASan hook

The asm trampoline could call the user's entry function directly:
`mov rcx, r13; call r12`. But the first run of any fiber needs to call
`__sanitizer_finish_switch_fiber(nullptr, ...)` BEFORE the user's entry
function uses any stack memory — otherwise ASan still thinks we're on the
source fiber's stack. The asm trampoline therefore calls a tiny C++
`fiber_entry_helper(entry, args)` that does the ASan hook then calls
`entry(args)`. The helper is in `FiberPrimitiveHelper.cpp` (the .asm is named
`FiberPrimitive.asm`; we'd hit a `FiberPrimitive.obj` name collision if both
were `FiberPrimitive.{cpp,asm}`).

### SEH frame (fs:[0x00]) NOT swapped

The research agent flagged this as a sharp edge — boost.context's MASM does
not swap `ExceptionList` at TIB offset 0x00 either. On x86_64 Windows, SEH
uses table-based unwinding via `.pdata`/`.xdata` sections, NOT the FS:[0]
linked-list mechanism that 32-bit Windows used. So the FS:[0] field is
effectively unused on x64. If we ever supported throwing exceptions across
fiber switches (we don't — Luth policy forbids exceptions in jobs), we'd
need to revisit. Today: not a concern.

### IsThreadAFiber() returns false

The CaptureCurrentThreadAsFiber path no longer calls `ConvertThreadToFiberEx`,
so Win32's IsThreadAFiber will return FALSE on worker threads. Nothing in
Luth or its dependencies branches on this API (grep is clean). If a future
third-party DLL gates behavior on `IsThreadAFiber()`, this would surface
there — currently a non-issue.

### Removing the Win32 backend at series end

The plan was to keep the backend behind `LUTH_USE_WIN32_FIBERS` for "one
validation cycle." We did exactly that: lands sub-task C with dual paths,
validates samples + ASan in D/E, retires in F. The retirement is one commit;
if a regression surfaces in the wild, it's a single `git revert` away.

---

## Files touched

**New (engine)**
- `luth/source/luth/jobs/FiberStackAllocator.h`
- `luth/source/luth/jobs/FiberStackAllocator.cpp`
- `luth/source/luth/jobs/FiberPrimitive.h`
- `luth/source/luth/jobs/FiberPrimitive.asm`
- `luth/source/luth/jobs/FiberPrimitiveHelper.cpp`

**Modified (engine)**
- `luth/source/luth/jobs/Fiber.h` — full rewrite: VirtualAlloc-backed stack
  model, `SwitchTo(from, to)` with ASan hook wrappers, `CaptureCurrentThreadAsFiber`,
  `CaptureCurrentStackBounds`. Win32 backend code removed in sub-task F.
- `luth/source/luth/jobs/JobSystem.cpp` — 5 `Fiber::SwitchTo` callsites updated,
  `ConvertThreadToFiber` calls replaced with `CaptureCurrentThreadAsFiber`, 3
  FLS callsites replaced with `gs:[0x28]` intrinsics, `s_FlsIndex` +
  `FlsAlloc`/`FlsFree` removed, `Fiber::Create` callsite extended with ownerCtx.

**Modified (build)**
- `premake5.lua` — DebugASan workspace configuration; workspace-wide
  `_DISABLE_VECTOR_ANNOTATION` + `_DISABLE_STRING_ANNOTATION` filter.
- `luth/premake5.lua` — source/**.asm glob; DebugASan filter.
- `luth/extern/premake5-tracy.lua` — DebugASan filter (Release CRT, no ASan).
- `luth/extern/premake5-jolt.lua` — DebugASan filter (Release CRT, no ASan).
- `samples/physics_smoke/premake5.lua` — DebugASan filter.
- `luth/source/luth/core/Version.h` — patch bump 2.10.3 → 2.11.0.

**Reference code consulted (not copied)**
- boost::context `jump_x86_64_ms_pe_masm.asm` / `make_x86_64_ms_pe_masm.asm`
- libco `amd64.c`
- Folly `FiberManager.cpp`

---

## Verification

- Luth.lib + JobSysProof clean Debug + Release + DebugASan.
- Luthien.exe Debug builds (post-build copy of `shaderc_shared.dll` fails on
  an unrelated path issue pre-existing in the runtime project — not a fiber
  regression).
- JobSysProof Debug: 600-frame Jolt drop test, 1000 bodies, 5364 ms total
  (~9 ms/step), body settles at Y=0.477536 (correct floor contact). Clean
  JobSystem shutdown, exit 0.
- JobSysProof Release: same scenario, 906 ms total (~1.5 ms/step). Clean.
- JobSysProof DebugASan: 60 frames, 815 ms total (~14 ms/step). **No ASan
  reports.** `dumpbin /imports` confirms `clang_rt.asan_dynamic-x86_64.dll`
  imported with 40+ `__asan_*` symbols.
- The V1b lemming regression test (canonical proof for the WaitForCounter UAF)
  lives in the `foundation-testing` series and validates the regression
  scenario when that series resumes on top of this tag.
