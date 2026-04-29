# v2.8.11 — slot-alloc-spinlock

**Date:** 2026-04-29
**Commits:** 3 (on `refactor/slot-alloc-spinlock`)
**Issue:** [#104](https://github.com/Hekbas/Luth/issues/104)

---

## Overview

Closed the v2.8.4 D6 carry-over. `MaterialSystem::m_Lock` and `BoneMatrixBuffer::m_Lock` move from `std::mutex` to `Luth::SpinLock`, aligning both with cornerstone #1 (no `std::mutex` on hot paths). `gpu-tagged-heap` (v2.8.10) was the precondition: the per-frame upload that previously lived under the lock — and made `std::mutex` justifiable as "long-running, can't spin" — is now an allocator-vended region copy, so the lock only protects the slot-alloc primitive operations.

Doc sweep folded into the wrap-up: `arch/memory.md` gains a `GPUTaggedPageAllocator` row in the allocator table and a section describing the V6 driver wiring shared by both heaps; `arch/rendering-pipeline.md` updates Sets 2/4/5 from "Per frame if dirty" to "Per stage — rebound to fresh tagged-heap region (UPDATE_AFTER_BIND)"; `arch/fiber-system.md` V6 row notes both halves of the Onion/Garlic split are operational; `ARCHITECTURE.md` System Hierarchy and Design Philosophy mention the GPU heap; `BACKLOG.md` records `gpu-tagged-heap` and this epic as shipped, bumps `shader-reload-async`/`vulkan-polish` versions, and threads the new nodes through the dependency graph.

Tag-only release per the v2.8.5 internal-architecture policy.

---

## Lock duration analysis

| Lock site | Pre-v2.8.10 hold time | Post-v2.8.10 (this) | V1 fit |
|---|---|---|---|
| `MaterialSystem::RegisterMaterial` | < 100 cycles (deque pop + slot write) | unchanged | ✅ |
| `MaterialSystem::UnregisterMaterial` | < 100 cycles (deque push + slot clear) | unchanged | ✅ |
| `MaterialSystem::Update` | ~50 µs (16384-slot iter + memcpy to 2.3 MB persistent SSBO + `dirtyFramesRemaining` countdown) | ~25 µs (16384-slot iter + memcpy to allocator-vended region) | ⚠️ borderline; no contention today |
| `BoneMatrixBuffer::AllocateBlock` | < 100 cycles (deque pop) | unchanged | ✅ |
| `BoneMatrixBuffer::FreeBlock` | < 100 cycles (deque push) | unchanged | ✅ |
| `BoneMatrixBuffer::UploadBones` | none (was lock-free, disjoint ranges) | unchanged | n/a |
| `BoneMatrixBuffer::Update` | n/a (didn't exist) | none (lock-free copy from CPU staging) | n/a |

`MaterialSystem::Update`'s 25 µs is the only borderline entry. Strict V1 says < 100 cycles; in practice contention is zero — `EnsureMaterialRegistered` (game stage) and `Update` (game stage) both run inside `RenderSnapshot::Capture`'s sequential flow, so no second fiber tries to acquire the lock during the 25 µs window. If a future epic parallelizes register paths, the lock can be further shrunk via an atomic free-list.

---

## Sub-tasks

| # | Slug | Commit | Notes |
|---|---|---|---|
| A | MaterialSystem mutex → SpinLock | `8f43eaa` | `std::mutex` → `Luth::SpinLock`; three `std::lock_guard<std::mutex>` → `SpinLockGuard` (Register, Unregister, Update); comment block updated to reflect the new hold-time / contention story |
| B | BoneMatrixBuffer mutex → SpinLock | `13c522a` | `std::mutex` → `Luth::SpinLock`; two `std::lock_guard<std::mutex>` → `SpinLockGuard` (AllocateBlock, FreeBlock); UploadBones / Update were already lock-free under v2.8.10 |
| C | Doc sweep + wrap-up | (this commit) | `arch/memory.md`, `arch/rendering-pipeline.md`, `arch/fiber-system.md`, `ARCHITECTURE.md`, `BACKLOG.md`, `ROADMAP.md`. Version bump + history file |

---

## Build verification

Solution unchanged structurally; rebuilt Debug x64 after each sub-task. No new warnings, no validation errors. Smoke test (60s soak with material slider drag + skinned-mesh pose edits + camera orbit) — no flicker, no tearing, no validation errors.

---

## Out of scope

- Atomic free-list / lock-free slot allocation. Defer until parallel-register concurrency is real.
- Other `std::mutex` instances elsewhere in the engine. The v2.8.4 D6 deferral was specific to `MaterialSystem` and `BoneMatrixBuffer`; this epic closes exactly that scope.
