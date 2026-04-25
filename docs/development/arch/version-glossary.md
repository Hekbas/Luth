# Version Markers (V1-V6) — Glossary

Source comments throughout the engine reference `Vn` markers. There are **two unrelated namespaces** with the same prefix — context disambiguates them. This doc indexes every site and points to the authoritative spec.

---

## Two namespaces

| Namespace | Meaning | Authoritative doc |
|-----------|---------|-------------------|
| **JobSystem hazards** | V1-V6 are *vulnerability mitigations* in the fiber scheduler design — each `Vn` is a class of bug the architecture defends against | [`arch/fiber-system.md`](fiber-system.md) |
| **Asset format versions** | V1, V2, … are *binary schema versions* of artifact files (`.luth.model`, `.luth.shader`) — `Header.Version` field controls reader path | [`arch/asset-pipeline.md`](asset-pipeline.md) |

Comments referencing either should use the form `// V3 (job hazard) — see arch/version-glossary.md` so a reader can disambiguate without context.

---

## Namespace 1 — JobSystem hazards

Each `Vn` is the engine's response to a known fiber-scheduling pitfall. Full detail in [`arch/fiber-system.md`](fiber-system.md).

| Hazard | One-line meaning | Where in source |
|--------|------------------|-----------------|
| **V1** | No yield under spin-lock; hot path uses `SpinLock`, never `std::mutex` | [`jobs/SpinLock.h:9`](../../../luth/source/luth/jobs/SpinLock.h), [`core/FrameData.h:74`](../../../luth/source/luth/core/FrameData.h) |
| **V2** | Main thread isolated — never steals jobs, never yields | [`core/App.cpp:146,152`](../../../luth/source/luth/core/App.cpp), [`jobs/JobSystem.cpp:617,709`](../../../luth/source/luth/jobs/JobSystem.cpp) |
| **V3** | RAII `RecordingScope` blocks fiber yield while a `VkCommandBuffer` is being recorded | [`jobs/JobSystem.h:95,107,158`](../../../luth/source/luth/jobs/JobSystem.h), [`jobs/JobSystem.cpp:711`](../../../luth/source/luth/jobs/JobSystem.cpp), [`jobs/Fiber.h:117`](../../../luth/source/luth/jobs/Fiber.h) |
| **V4** | `WaitOnAddress` + generation counter pattern — every queue insertion pairs with `WakeByAddressSingle` | [`jobs/MPMCQueue.h:18,73,118,147`](../../../luth/source/luth/jobs/MPMCQueue.h), [`jobs/JobSystem.cpp:248,435,438`](../../../luth/source/luth/jobs/JobSystem.cpp) |
| **V5** | Depth-limited inline execution — `WaitForCounter` runs work inline up to depth 4, then forces fiber switch | [`jobs/JobSystem.h:98,153`](../../../luth/source/luth/jobs/JobSystem.h), [`jobs/JobSystem.cpp:33,640`](../../../luth/source/luth/jobs/JobSystem.cpp), [`renderer/rendergraph/RenderGraph.{h,cpp}`](../../../luth/source/luth/renderer/rendergraph/RenderGraph.h) |
| **V6** | Overflow `TaggedPageAllocator` tier — Frame N gets pages even when GPU stalls on N-2 | [`core/FrameData.h:66`](../../../luth/source/luth/core/FrameData.h) |

When adding new hot-path code in `jobs/` or `renderer/rendergraph/`, comment any new construct that addresses one of these hazards as `// V<n> ...`. Don't invent V7+ without first updating `arch/fiber-system.md`.

---

## Namespace 2 — Asset format versions

Binary artifact files (under `cache/assets/`) store a `Header.Version` field at the top. Importers/serializers branch on it to support old artifacts without forced re-import. Defined in [`luth/source/luth/resources/AssetSerializer.{h,cpp}`](../../../luth/source/luth/resources/AssetSerializer.h).

### Model artifacts (`.luth.model`)

| Version | Schema | Notes |
|---------|--------|-------|
| **V1** | `ModelHeader` = `{MeshCount, MaterialCount}` (8 B); `MeshHeader` = `{VertexCount, IndexCount, MaterialIndex}` (12 B) | Original format. Read-only support for backward compat (`AssetSerializer.cpp:302`). |
| **V2** | `ModelHeader` adds `IsSkinned`, `BoneCount`, `AnimationCount`; payload appends Skeleton + AnimationClips | Current write format (`AssetSerializer.cpp:190`). All new model imports use V2. |

`AssetSerializer.cpp:302–327` is the V1 backward-compat read branch — preserved so old cached artifacts don't trigger re-import on engine upgrade.

### Shader artifacts (`.luth.shader`)

| Version | Schema | Notes |
|---------|--------|-------|
| **V1** | Paired `.vert` + `.frag` artifacts (legacy) | **Rejected.** Pre-v2.1.0 format; current importer rejects V1 artifacts (`AssetSerializer.cpp:411`). |
| **V2** | Single-stage shader — one source file → one artifact + UUID | Current format since v2.1.0 `shader-asset-pipeline` epic. |

V1 shader artifacts no longer round-trip — re-import is required if cache is from before v2.1.0.

---

## Conventions for new comments

| Form | When |
|------|------|
| `// V3 (job hazard) — see arch/version-glossary.md` | Inside `jobs/`, `renderer/rendergraph/`, hot paths that touch fiber scheduling |
| `// V2 (asset format)` | Inside `resources/AssetSerializer.{h,cpp}` or any importer branching on header version |
| `// V<n> ...` (no qualifier) | **Avoid.** Always disambiguate the namespace |

If a future epic introduces a third namespace (e.g. shader header schema bumps), add a section to this file before sprinkling `Vn` comments in source.
