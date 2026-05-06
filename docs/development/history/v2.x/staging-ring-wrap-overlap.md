# v2.9.10 — staging-ring-wrap-overlap

**Date:** 2026-05-05
**Commits:** 2 (on `fix/staging-ring-wrap-overlap`)
**Issue:** [#119](https://github.com/Hekbas/Luth/issues/119)

---

## Overview

`UploadContext::AllocateStaging` had two boundary-condition bugs that produced intermittent texture corruption — partial pixel mixing between two unrelated textures, only on burst loads. Symptom: an albedo texture rendered with bands or patches of normal-map data interleaved through it.

Both bugs reduce to misclassifying full-ring states as "linear, free space ahead." `memcpy` then overwrote staging bytes the GPU was still reading via `vkCmdCopyBufferToImage`, producing the visible mosaic.

---

## Bug 1: wrap-with-tail-at-zero (commit `323045b`)

Trace: three textures (~21 MB each) fill the ring at `0..63M`, none retired. A fourth allocation wraps — `alignedHead = 0`, `m_StagingHead = 0`. Front block's offset is 0, so `m_StagingTail = 0`. The old space check `alignedHead >= m_StagingTail` evaluates `0 >= 0` → true → "linear, free space ahead" → allocates at offset 0, overlapping the in-flight first block.

Fix: track wrap explicitly per call (`bool wrapped`) instead of inferring it from `alignedHead < m_StagingTail`. Wrapped allocations route through the bounded "must fit before tail" branch. Added `LH_CORE_ASSERT(size <= STAGING_SIZE)` defensive guard at the top, and an `!hasSpace && empty` defensive assert (unreachable today; surfaces a `STAGING_SIZE` regression cleanly).

---

## Bug 2: head-wraps-exactly-to-tail (commit `d9362e8`)

Surfaced after Bug 1's fix shipped — user reported the same symptom recurring on a different texture pair. Adjacent edge case the first fix missed.

Trace: T1 retires, T4 wraps to `0..21M`. blocks (FIFO) `= [T2(21M), T3(42M), T4(0)]`. `m_StagingHead = 21M`, `m_StagingTail = T2's offset = 21M`. **head == tail, ring full.** T5 (5M) request: `alignedHead < m_StagingTail` is `21M < 21M` → false, falls into the linear else branch → `hasSpace = true`. Allocates at 21M, overlapping in-flight T2.

Fix: add explicit `alignedHead == m_StagingTail` full-ring detection before the linear/wrapped split. Routes through the wait/recurse path.

---

## Why two commits

The two bugs differ in shape (`tail = 0` vs. `tail > 0` post-wrap-allocation), and Bug 1's trace happened to use blocks where the wrap landed at `tail = 0` — the symmetric case didn't surface until the user ran the binary. Two commits keep bisect signal clean: revert `d9362e8` alone if Bug 2's check turns out to over-trigger, revert `323045b` alone for the original wrap-overlap.

---

## Architectural alignment

- **Memory primitives (cornerstone #1):** Composes with the existing `UploadContext` ring; no new primitive. Fence-tagged staging lifetime remains structurally distinct from `GPUTaggedPageAllocator`'s frame-tagged model — `texture-async-uploads.md` reached the same conclusion when it extended the ring rather than migrating it.
- **No `std::mutex` on hot paths:** `UploadContext::m_Lock` is asset-load-time; carved exception per `texture-async-uploads.md`. Out of scope.

---

## Build verification

Debug x64 builds clean (0 errors, pre-existing warnings only). User smoke-test: scene reload of a multi-PBR-textured model that previously reproduced banded mixing rendered correctly across multiple consecutive opens.
