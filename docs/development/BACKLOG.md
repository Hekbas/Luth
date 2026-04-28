# Luth Engine — Backlog

> Definitive plan for planned epics. Each section expands the short entry in [`ROADMAP.md`](ROADMAP.md). Effort scale: **S** (hours) / **M** (½–1 day) / **L** (2–5 days) / **XL** (multi-week).

---

## Architecture Snapshot (v2.8.2)

What exists today — directly shapes sequencing for remaining work.

| Capability | Status | Notes |
|-----------|--------|-------|
| Render Graph (DAG, barriers, culling) | ✅ Graphics + compute + buffers | Shipped in `compute-gpu-culling` (v1.2.0) |
| Compute shaders | ✅ Formal pipeline, render-graph integrated | `VulkanComputePipeline` + pipeline cache |
| Draw submission | ✅ `vkCmdDrawIndexedIndirect` everywhere | GPU frustum cull |
| Buffer resources | ✅ SSBO tracked in render graph | `ResourceState::ComputeRead/Write`, `StorageBuffer` |
| Queue | ✅ Single graphics queue (compute on same) | Async compute = future |
| Forward PBR | ✅ 3 render modes, 6 descriptor sets | Forward+ extends Set 3 |
| Shadows | ✅ 4-cascade PSSM, per-cascade GPU cull | Shipped in `csm` (v1.3.0) |
| GTAO | ✅ Half-res compute, Jimenez 2016 | Shipped (v1.5.0) |
| Animation | ✅ Sampling, blending, root motion, GPU skinning | Jiggle bones slot in after sampling |
| Play mode | ✅ Editor state machine + scene snapshot | Shipped (v2.8.0) |
| Game panel | ✅ Letterbox + first-Camera entity | Shipped (v2.8.1) |
| Profiling | ✅ Tracy CPU + GPUTimerPool | See [arch/profiling.md](arch/profiling.md) |
| Memory tracking | ✅ Categorized engine + Tracy STL hooks | See [arch/memory.md](arch/memory.md) |
| Undo/redo | ✅ 14 command types | New components need new command types |
| Scene serialization | ✅ JSON `.luth` | New components need serializer entries |
| Frame debugger | ✅ Per-draw archive + replay-then-copy, frozen-state | Shipped in `frame-debugger-sync` (v1.4.0) |

---

## Dependency Graph

```mermaid
graph TD
    consol["engine-consolidation"]
    fdp["frame-debugger-polish"]
    vc["vulkan-correctness"]
    aqp["animation-quick-pass"]
    pbr["persistent-buffer-ring"]
    sra["shader-reload-async"]
    vp["vulkan-polish"]
    jolt["jolt-physics"]
    jiggle["jiggle-bones"]
    acq["async-compute-queue"]
    rga["rg-aliasing"]
    forward["forward-plus"]
    fxaa["fxaa-taa"]
    av2["animation-controller-v2"]
    particles["gpu-particles"]
    future["future"]

    vc --> pbr
    vc --> sra
    vc --> vp
    vc --> acq
    aqp --> av2
    acq --> forward
    forward --> particles
    jolt --> jiggle
    fxaa --> future
    av2 --> future

    style consol fill:#2563eb,color:#fff
    style fdp fill:#2563eb,color:#fff
    style vc fill:#2563eb,color:#fff
    style aqp fill:#2563eb,color:#fff
    style pbr fill:#2563eb,color:#fff
    style sra fill:#2563eb,color:#fff
    style vp fill:#2563eb,color:#fff
    style jolt fill:#7c3aed,color:#fff
    style jiggle fill:#7c3aed,color:#fff
    style acq fill:#7c3aed,color:#fff
    style rga fill:#7c3aed,color:#fff
    style forward fill:#7c3aed,color:#fff
    style fxaa fill:#7c3aed,color:#fff
    style av2 fill:#7c3aed,color:#fff
    style particles fill:#dc2626,color:#fff
    style future fill:#525252,color:#fff
```

> 🔵 Near-term · 🟣 Mid-term · 🔴 Advanced · ⚫ Future

---

## Epic: `engine-consolidation` — v2.8.2

> **Audit-driven housekeeping pass before resuming feature work.**

Roadmap restructure (Effort scale, terse summaries), four new arch sub-docs (memory / profiling / validation-layers / version-glossary), comment-banner sanitization, V1-V6 cross-ref standardization, Tracy memory hooks for STL/heap, Tracy CPU coverage expansion across editor panels and RG passes.

**Effort:** M

---

## Epic: `animation-quick-pass` — v2.8.8

> **Status: Shipped v2.8.8** — see [`history/v2.x/animation-quick-pass.md`](history/v2.x/animation-quick-pass.md).

Decoupled animation clips from `Model`: clips became first-class UUID-addressable assets, `ModelImporter` emits one `.anim` per clip, runtime + scene + inspector switched to UUID handles. Preview-toggle UX (originally listed) was deferred to a future polish item.

---

## Epic: `persistent-buffer-ring` — v2.8.9

> **Status: Shipped v2.8.9** — see [`history/v2.x/persistent-buffer-ring.md`](history/v2.x/persistent-buffer-ring.md).

Triple-buffered the three persistent CPU-mapped SSBOs (ObjectSSBO Set 5, IndirectBuffer, Material SSBO Set 2) with single-buffer slice math. Cull compute gained a `srcOffset` push-constant; Material dirty-frame countdown propagates a single mutation across slices. VMA modernized off deprecated `CPU_TO_GPU` flag to `AUTO + HOST_ACCESS_SEQUENTIAL_WRITE_BIT + MAPPED_BIT` with per-slice `vmaFlushAllocation` (no-op on coherent memory).

---

## Epic: `shader-reload-async` — v2.8.10

> **Drop the per-save `vkDeviceWaitIdle`. Defer old-pipeline destroy through the (V1 SpinLock-safe) deletion queue.**

ShaderWatcher's reload callback currently runs `vkDeviceWaitIdle` on every `.vert`/`.frag`/`.comp` save and rebuilds every pipeline that consumes the SPV. The poll runs at the top of every view's `Execute` (twice per frame when both Scene and Game viewports are open). Frame-pacing penalty is real even when nothing changed.

| Area | Detail |
|------|--------|
| Poll site | Move `m_ShaderWatcher.Poll()` from `RenderPipeline::Execute` to a once-per-frame call site (`RenderingSystem::Update` prologue) |
| Deferred destroy | Reload stashes old `VkPipeline` + `VkShaderModule` on `VulkanContext::PushDeletion` (now thread-safe under SpinLock); builds new ones; swaps atomically — no `vkDeviceWaitIdle` needed |
| DescriptorAllocator | Wire `Reset()` per-frame OR delete the class — currently has no callers and accumulates unbounded |

**Dependencies:** `vulkan-correctness` ✅
**Effort:** S

---

## Epic: `vulkan-polish` — v2.8.11

> **Tier-2/3 cleanup: transient cache hygiene, async asset uploads, sampler/usage corrections.**

Last of the foundation-stabilization renderer efforts before `jolt-physics`. None of these are correctness bugs; collectively they tighten the cost model for forward+ and gpu-particles.

| Area | Detail |
|------|--------|
| `RenderResourceCache` | Hash key by `(w, h, format, usage)` (currently linear scan, ignores usage flags); trim stale-frame threshold from 10 000 to ~30; usage-aware allocation so future transient compute targets fit |
| Async asset uploads | Wire `UploadContext` for runtime asset loads in `VKTexture` / `VKBuffer` ctors. Drop `ImmediateSubmit` to startup-only — runtime texture loads no longer block the calling thread |
| Bindless cleanup | Free-list to `vector<u32>` (LIFO); sentinel for "not registered" → `UINT32_MAX` (currently collides with the null-texture's slot 0) |
| Validation chain | Pass `VkDebugUtilsMessengerCreateInfoEXT` via `VkInstanceCreateInfo.pNext` so vkCreateInstance / vkDestroyInstance failures get reported |
| Hardcoded magic | Outline width/alpha and grid colors+fade pushed through `EditorSettings` instead of baked in shader push-constants |

**Dependencies:** `vulkan-correctness` ✅
**Effort:** M

---

## Epic: `jolt-physics` — v2.9.0

> **Rigid body physics.** Makes Luth a game engine, not just a renderer.

### Architecture: Jolt on CPU via Fiber Job System

Jolt exposes a `JPH::JobSystem` interface. Implement to dispatch onto our fiber scheduler:

```cpp
class LuthJoltJobSystem : public JPH::JobSystem {
    // Map Jolt jobs → Luth::JobSystem::Execute()
    // Map Jolt barriers → Luth::AtomicCounter
};
```

### Key Changes

| Area | Detail |
|------|--------|
| **New components** | `RigidBody` (mass, restitution, friction, body type), `Collider` (box/sphere/capsule/mesh shape) |
| **PhysicsSystem** | New ECS system: `Init()` creates Jolt world, `Update()` steps simulation, syncs transforms |
| **Transform sync** | After Jolt step: `JPH::BodyInterface::GetWorldTransform()` → `Component::Transform` |
| **Debug draw** | Wireframe collider visualization in editor |
| **Raycasting** | `PhysicsSystem::Raycast()` for mouse picking, gameplay queries |
| **Editor integration** | Inspector for RigidBody/Collider, play-mode-only simulation |
| **Serialization** | RigidBody + Collider component serialization in `.luth` scenes |

**Dependencies:** `play-mode` ✅
**Effort:** XL

---

## Epic: `jiggle-bones` — Secondary Physics

> **Custom Verlet spring simulation.** ~200 lines, not Jolt.

Runs after animation sampling, before bone matrix SSBO upload. Per-bone Verlet integration + distance constraints + optional sphere collider push-out.

### Key Changes

| Area | Detail |
|------|--------|
| **New component** | `JiggleBone { BoneName, Stiffness, Damping, Gravity, [runtime: CurrentPos, PreviousPos] }` |
| **Chain support** | `JiggleChain { RootBoneName, ChainLength, Stiffness, Damping }` for ponytails/capes |
| **AnimationSystem** | Add jiggle simulation step after global transform computation |
| **Inspector** | Stiffness/damping sliders, gravity vector, visualization gizmo |
| **Serialization** | New component in scene format |

**Dependencies:** Benefits from `jolt-physics` for collider sources; not strictly required
**Effort:** M

---

## Epic: `async-compute-queue` — v2.9.2

> **Run cull + GTAO on a dedicated compute queue so they overlap shadow rasterization on the graphics queue.**

Today every compute pass (5× cull, 3× GTAO) serializes on the single graphics queue. With a dedicated compute family + cross-queue timeline-semaphore sync, the cull dispatches and GTAO chain run alongside shadow-cascade rasterization for free. Prereq for `forward-plus` because clustered lighting adds two more compute passes (cluster-build, light-assign) that benefit from the same overlap.

| Area | Detail |
|------|--------|
| Queue discovery | Walk queue families post-instance; pick the first compute-only family (no graphics bit) for compute, optionally a transfer-only family for `UploadContext`. Fall back to the graphics queue if neither exists |
| Per-queue command pools | `CommandAllocatorPool` extended with queue-family parameter; one pool per (frame, queue family). V3 affinity preserved per queue |
| Cross-queue sync | Timeline semaphores signaled by compute submit, waited on by graphics submit (and vice versa for GTAO → geometry). Existing `m_FrameTimeline` stays per-frame; compute work uses a separate timeline value scheme |
| RG node queue selection | `PassNode::queue` enum (Graphics/Compute/Transfer); barrier-solver emits ownership-transfer barriers on cross-queue dependencies |
| Backend submit | `VulkanBackend::SubmitFrame` becomes per-queue submit; main-thread queue mutex contention rises slightly but is bounded |

Plan-mode this one before touching code — queue ownership transfers and timeline-semaphore design have several reasonable shapes.

**Dependencies:** `vulkan-correctness` ✅
**Effort:** L

---

## Epic: `rg-aliasing` (optional) — v2.9.3

> **Use the lifetime data the RG already computes to alias transient memory.**

`ResourceNode::firstPass`/`lastPass` are populated in `ComputeLifetimes` but never consumed for memory aliasing. Two transient resources with disjoint lifetimes can share VkImage/VmaAllocation; on a dense post-fx chain (bloom mip pyramid + GTAO half-res + selection mask) peak transient VRAM drops 30–50%.

Optional — defer if `forward-plus` doesn't push transient memory pressure on the target hardware. The `RenderResourceCache` rework in `vulkan-polish` already trims the steady-state churn; aliasing is the next step beyond that.

**Dependencies:** —
**Effort:** M

---

## Epic: `forward-plus` — Clustered Lighting

> **Remove the 64 point light ceiling.** Thousands of lights with minimal overhead.

Replace fixed `LightUBO` with GPU-driven light assignment via clustered shading.

| Pass | Type | Detail |
|------|------|--------|
| **Cluster build** | Compute | Divide view frustum into 16×9×24 clusters; compute cluster AABBs |
| **Light assign** | Compute | For each light, find overlapping clusters; write light index lists |
| **GeometryPass** | Fragment | Read cluster for current fragment → iterate assigned lights only |

### Key Changes

| Area | Detail |
|------|--------|
| **Light SSBO** | Replace `LightUniforms` (fixed 64 lights) with unbounded SSBO |
| **Cluster SSBO** | Per-cluster light index list + offset/count table |
| **PBR shader** | Read cluster ID from fragment position; iterate cluster's light list |
| **Point light cap** | Raise from 64 to unlimited (practical: thousands) |

**Dependencies:** `compute-gpu-culling` ✅, `async-compute-queue`
**Effort:** L

---

## Epic: `fxaa-taa` — Anti-Aliasing

> **FXAA quick win + TAA pairs with GTAO temporal accumulation.**

| Technique | Effort | Notes |
|-----------|--------|-------|
| **FXAA** | S | Single fullscreen post-process pass. Lottes 2011 GLSL impl. Drop-in after post-process. |
| **TAA** | M | Requires motion vectors (per-object velocity buffer), Halton jitter, history buffer, neighborhood clamping. Improves GTAO + removes temporal flickering. |

**Dependencies:** TAA needs per-object previous-frame transforms (pairs with `compute-gpu-culling` ✅)

---

## Epic: `animation-controller-v2` — State Machine + Blend Trees

> **Real animation graph.** State machine + 1D/2D blend trees + retargetable rigs. Scales to 30+ animations per character.

Successor to v1.0 `animation-system`. New asset format for the controller graph; runtime evaluator inside `AnimationSystem`; editor graph view.

### Key Changes

| Area | Detail |
|------|--------|
| **AnimationController asset** | New `.luth.controller` artifact: states, transitions, parameters, blend trees |
| **Runtime evaluator** | State machine ticks per entity; pose blending replaces direct clip sampling |
| **Retargetable rigs** | Skeleton remap when controller's source rig differs from instance's |
| **Editor graph view** | Node-based controller editor (panel) |
| **Inspector** | Per-entity controller binding + parameter exposure |

**Dependencies:** `animation-quick-pass` (rig/clip decoupling lands first)
**Effort:** XL

---

## Epic: `gpu-particles` — GPU Particle System

> **First fully GPU-driven simulation.** Compute emit → simulate → render via indirect draw.

| Pass | Type | Detail |
|------|------|--------|
| **Emit** | Compute | Spawn particles from emitter configs, write to particle SSBO |
| **Simulate** | Compute | Integrate positions, apply forces/gravity, decay lifetime, kill dead |
| **Compact** | Compute | Stream compaction to remove dead particles |
| **Render** | Graphics (indirect) | Point sprites or billboards via `vkCmdDrawIndirect` |

**Dependencies:** `compute-gpu-culling` ✅, `forward-plus` (for lit particles, optional)
**Effort:** L

---

## Future (Not Scoped)

Beyond planned epics, ordered roughly by value:

| Feature | Depends On | Notes |
|---------|-----------|-------|
| **Shadow frustum-union fit** | `game-panel` ✅ | CSM refits per view (2× shadow cost with game panel open); one union fit covers all active views |
| **Frame Debugger per-view capture** | `game-panel` ✅ | Capture currently scene-view-only; extend tracked RTs + archive slots to tag by view |
| **Auto-wire `GPUTimerPool` into RenderGraph** | — | Currently per-pass insertion is manual |
| **Scene-panel post-process toggle** | — | Unity-style toggle to disable bloom/tonemap/vignette for lookdev |
| **Tracked STL allocators (`LH::Vector<T>`)** | — | Closes the STL gap on `MemoryTracker` (currently covered only by Tracy hooks) |
| **HZB Occlusion Culling** | `compute-gpu-culling` ✅ | Two-phase cull pipeline; depth pyramid as compute |
| **Prefab System** | `play-mode` ✅, `jolt-physics` | Entity templates with override tracking |
| **Scripting (C# or Lua)** | `play-mode` ✅, `jolt-physics` | Requires play mode + physics for meaningful scripts |
| **Asset Streaming** | `compute-gpu-culling` ✅ | Async GPU upload via transfer queue |
| **Deferred GBuffer** | `forward-plus` | If Forward+ hits limits; unlocks SSR, decals |
| **SSR (Screen-Space Reflections)** | GBuffer or depth | Hi-Z traced reflection |
| **Volumetric Fog** | `compute-gpu-culling` ✅, `forward-plus` | Froxel-based, compute-driven |
| **3D Spatial Audio** | `play-mode` ✅ | Orthogonal; integrate when demo needs it |
| **Ragdoll** | `jolt-physics`, `animation-controller-v2` | Bone driven by Jolt body during ragdoll state |
| **Animation v3 (DQS, IK, morph targets)** | `animation-controller-v2` | Beyond state machine + blend trees |
| **Visual Shader Editor** | — | Editor luxury, low priority |
