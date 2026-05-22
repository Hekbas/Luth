# rt-renderer.1-bindless — bindless-migration

**Date:** 2026-05-22
**Commits:** 6 (on `refactor/bindless-migration`)
**Issue:** [#128](https://github.com/Hekbas/Luth/issues/128)
**Umbrella:** [#127](https://github.com/Hekbas/Luth/issues/127)
**Series:** `rt-renderer`, first effort. Mode A series-coalesced — checkpoint tag, no `Version.h` bump.

---

## Overview

First sub-effort of the `rt-renderer` v3.0.0 arc. Closes the remaining gaps so downstream efforts (forward-plus cluster shadows, RT hit shaders, ReSTIR material sampling) can assume a uniform bindless model from day one.

Most of the bindless machinery was already in place: `BindlessDescriptorSet` (Set 1 binding 0, 16384 combined image-samplers, partial-bound + UAB) ships since v1.0.0; the deferred-bindless-registration pump on `UploadContext` (v2.8.14) absorbs the async-upload-fence race; `pbr.frag` already samples `sampler2D globalTextures[]` with `nonuniformEXT`; `Material::GPUMaterialData` already stores `u32` indices for the four primary maps. The actual work was narrower than the issue title suggests:

1. **`bufferDeviceAddress` enablement.** The feature was off — buffers couldn't even emit BDA pointers, let alone consume them. Foundation for cluster-light SSBO addressing (A.3) and RT hit-shader mesh access (B.2).
2. **Set 1 binding 1 — canonical sampler array.** A 32-slot pure-sampler binding alongside the combined image-samplers, with 4 canonical samplers (linear/nearest × repeat/clamp) at fixed slots for shader paths that want sampler choice independent of the texture's baked one. Provisioned only — `pbr.frag` declares it but doesn't yet rewire sampling through it.
3. **`GPUMaterialData` completeness.** Only 4 of 9 `MapType` enum values flowed to the GPU struct. Extended to 8 slots (emissive / alpha / specular / thickness added; metallic + roughness still share one slot per glTF convention) + flag-bit repack to make room for new HAS_* bits without colliding with the existing UV-index packing.
4. **`thumbnail_mesh.frag` migration.** The one shader still using per-bake `vkUpdateDescriptorSets`. Port to bindless Set 1 sampling + push-constant-supplied diffuse index. Eliminated two private descriptor pools/layouts (bake set + inspector ring sampler set) — net ~120 LOC removed.
5. **Docs.** `arch/rendering-pipeline.md` Set 1 row + BDA subsection. Progress Tracker.

User-decided design boundaries (Phase 3 clarifications):
- BDA: cache addresses + flag-flip only. No shader path consumes BDA in A.1. Consumer arcs (A.3, B.2) pick their plumbing strategy (Set 5 ObjectData growth vs push constant vs SBT).
- Sampler in material: provision binding only. Don't add any `samplerIndex` field to `GPUMaterialData` — A.2 (slim-gbuffer) decides per-material vs per-map sampler choice when an actual shader path consumes a sampler index.
- Flag repack: HAS_* bits 0-7 (existing bits 0-4 unchanged; new bits 5-7 for alpha/specular/thickness), UV indices shifted 8-15 → 16-23.

Plan-mode validation surfaced no architectural deviations — every new mechanism composes with an existing primitive (`BindlessDescriptorSet`, `UploadContext` pump, `GPUTaggedPageAllocator`, `VKVertexBuffer`/`VKIndexBuffer` ctor pattern). No new files in `luth/source/luth/memory/` or `luth/source/luth/jobs/`. No new descriptor sets. No new sync primitives.

---

## Sub-tasks

| # | What landed | Commit |
|---|---|---|
| A | **`bufferDeviceAddress` device feature + VMA bit.** `features12.bufferDeviceAddress = VK_TRUE` in `VulkanContext::CreateLogicalDevice` (no extension request needed — Vulkan 1.2 core). Added to the `avail12.*` validation block + log spew so device selection fails loud on hardware that lacks the feature. `VMA_ALLOCATOR_CREATE_BUFFER_DEVICE_ADDRESS_BIT` on `VmaAllocatorCreateInfo.flags`. No buffer-side changes — pure feature-enable so the bisect cleanly separates "feature on" from "buffers use it". | [`477cded`](../../../../commit/477cded) |
| B | **`SHADER_DEVICE_ADDRESS_BIT` on mesh + tagged-heap buffers.** Usage bit added to `VKVertexBuffer` / `VKIndexBuffer` ctor info, and to `GPUTaggedPageAllocator`'s backing-buffer + large-one-shot creation. `VkDeviceAddress m_DeviceAddress` cached on the two mesh-buffer classes via `vkGetBufferDeviceAddress` post-`vmaCreateBuffer`; `GetDeviceAddress()` accessor exposed. Addresses are dormant — no shader consumes them yet. | [`d559131`](../../../../commit/d559131) |
| C | **`GPUMaterialData` extended to 8 map slots + flag repack.** `emissiveIndex` / `alphaIndex` / `specularIndex` / `thicknessIndex` added (`u32` each); struct grew 48 B → 64 B, still 16-byte aligned. `Material::UpdateGPUData` extended to resolve all 8 via the existing `GetIndex` lambda. Flags: existing HAS_NORMAL/METALROUGH/OCCLUSION/DIFFUSE/EMISSIVE bits 0-4 left in place so shader behavior is byte-identical; new HAS_ALPHA/SPECULAR/THICKNESS at bits 5-7; UV indices shifted from bits 8-15 → bits 16-23 (4 UV slots × 2 bits, same as today — preserves per-map UV selection for diffuse/normal/metalrough/occlusion). `pbr.frag` struct mirror + `FLAG_HAS_*` + `UV_SHIFT_*` constants updated. New fields sit dormant — `pbr.frag` does not yet sample emissive/alpha/specular/thickness. | [`0f05efe`](../../../../commit/0f05efe) |
| D | **Set 1 binding 1 — canonical bindless sampler array.** `BindlessDescriptorSet` gains a second binding (`VK_DESCRIPTOR_TYPE_SAMPLER`, 32 slots, partial-bound + UAB) on the same descriptor set. Layout + pool + binding-flags arrays extended from 1 entry to 2. New `CanonicalSampler` enum (`LinearRepeatAnisoMip` / `LinearClampAnisoMip` / `NearestRepeatNoMip` / `NearestClampNoMip`) at fixed slots 0-3, created via `CreateCanonicalSamplers()` and bound at Init. `BindSampler` / `UnbindSampler` LIFO over the remaining 28 slots (4..31). `pbr.frag` declares `layout(set=1, binding=1) uniform sampler bindlessSamplers[]` — present but unused; A.2 wires shader sampling through it. Existing `std::mutex` on the bindless set (the documented exception to the SpinLock rule) is preserved — sampler-array ops use the same lock for consistency. | [`c81fa63`](../../../../commit/c81fa63) |
| E | **`thumbnail_mesh` to bindless sampling.** Shader: `albedoTex` (set=0 binding=0, per-bake updated) → `globalTextures[diffuseIndex]` (Set 0 here, same layout as main pipeline's Set 1). PC grew 80 B → 96 B (added `u32 diffuseIndex` + padding to 16-byte align). `ThumbnailPreviewScene` drops two private descriptor pools (the bake-path single set + the inspector ring's per-slot sets), `CreateSamplerDescriptor` / `UpdateSamplerDescriptor` / `UpdateInspectorSamplerSet` deleted. New `ResolveBindlessIndex` helper coerces unregistered textures to slot 0 via `BindlessOrNull`. `CreateWhiteTexture` + `BakeMaterial` + `RenderMaterialInspector` call `UploadContext::DrainPendingBinds()` after their `vkDeviceWaitIdle` so the bindless slot is populated before the shader reads it. Net –120 LOC. | [`a6d86ff`](../../../../commit/a6d86ff) |
| F | **Wrap-up.** This history file. `arch/rendering-pipeline.md` Set 1 row updated + BDA pattern subsection added. `rt-renderer.md` Progress Tracker A.1 row → done. No `Version.h` bump (Mode A intermediate effort). `--no-ff` merge into `main` + `rt-renderer.1-bindless` tag. | this commit |

---

## Architectural decisions

### Combined image-samplers stay; pure sampler array layered alongside

`BindlessDescriptorSet`'s binding 0 was already `VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER` — every texture's bindless slot carries both view and sampler baked in at registration time, derived from the per-asset `TextureSettings`. Splitting this to a pure-`texture2D[]` + pure-`sampler[]` pair (the "modern bindless" idiom) would force a rewrite of every `BindTexture` call site, every shader-side sampling, and the deferred-bindless pump — all to satisfy an abstract preference for the split model. Plan-mode discipline says compose with existing primitives.

The adopted scheme is additive: keep binding 0 as combined image-samplers (existing pattern works, every shader's sampling stays correct), add binding 1 as a pure `VK_DESCRIPTOR_TYPE_SAMPLER` array for **future** shader paths that want sampler choice independent of texture. A.1 only provisions the binding + populates the 4 canonical samplers. A.2 (slim-gbuffer) is the first place that's likely to want it.

### Mesh BDA: cache at ctor, defer shader plumbing to consumers

`vkGetBufferDeviceAddress` is called once per mesh buffer at ctor time and cached on `VKVertexBuffer::m_DeviceAddress` / `VKIndexBuffer::m_DeviceAddress`. The address sits dormant until A.3 forward-plus cluster shadows or B.2 RT hit shaders need it. At that point the consumer arc picks the plumbing strategy — most likely two `uint64_t` fields per draw on `GPUObjectData` (Set 5), but per-draw push constant or SBT inclusion are equally reasonable and the choice depends on the consumer's access pattern.

A.1 avoiding shader plumbing buys two things: (1) zero risk of a half-migration where some draws use BDA and others use vertex input — that's a behavior delta worth bisecting cleanly when it lands; (2) the consumer arc owns its full migration scope, with the foundation already in place.

### Material flag-bit packing: preserve existing bits, shift UV indices to bits 16-23

The pre-change packing put HAS_* bits at 0-4 and UV indices at 8-15. Extending to 8 HAS_* bits would have collided with bit 8 (the diffuse UV index's LSB). Two ways to resolve:

- **Repack everything** (bits 0-8 = HAS_*, bits 9-15 = ???, bits 16-23 = UV). Cleanest layout but every shader bit reference moves.
- **Preserve existing bits, shift only UV** (bits 0-4 = existing HAS_*, bits 5-7 = new HAS_*, bits 16-23 = UV). Existing PBR shader behavior is byte-identical because bits 0-4 keep their semantics.

Option 2 won because A.1's verify gate is "render byte-identical" — Option 1 would have invalidated that gate for no benefit. The 8-bit gap between HAS_* and UV indices (bits 8-15) is reserved for future HAS_* bits or other flag-word users.

### Repack UV → bits 16-23, not bits 9-16

The alternative "pack UV indices immediately after HAS_*" (bits 9-16) is tighter but couples the two regions. If HAS_* ever needs to grow past 9 bits (more MapTypes), UV indices have to move again. Bits 16-23 leaves a 7-bit cushion (bits 9-15) for HAS_* growth without touching UV.

### `samplerIndex` field on `GPUMaterialData` deferred to A.2

The Plan agent initially proposed adding a per-material `samplerIndex` (one `u32`) to give A.2+ shaders something to consume. User pushback ("a little lost with all this... will materials become more complex?") surfaced a YAGNI dimension: A.1's shaders don't consume sampler indices either way. Adding a struct field now locks in the per-material-vs-per-map sampler-granularity question before A.2 has visibility into what its actual workflow needs.

Final decision: **provision the Set 1 binding 1 array** (visible from any future shader, populated with canonical samplers), **don't touch `GPUMaterialData` on the sampler front**. A.2 decides per-material vs per-map vs no-samplerIndex-at-all when an actual shader path consumes a `samplerIndex`. Zero rework either way — adding 1 field or 9 fields later is mechanical.

### `descriptorBindingUpdateUnusedWhilePending` not enabled

This binding flag is **not** implied by UPDATE_AFTER_BIND. It's required only when the application updates a descriptor binding while the descriptor set is in use by a vkCmdBind that hasn't been consumed yet. Today's `BindlessDescriptorSet` writes (via `BindTexture` / `UnbindTexture` / new `BindSampler` / `UnbindSampler`) happen outside frame submission — texture upload completion in the deferred-bind pump, or VKTexture dtor. None of those overlap a pending submission's recording.

A.3 may want to update Set 1 bindings from a job mid-frame (e.g. light-cluster-keyed bindless updates). At that point, add the flag preemptively. A.1 doesn't need it; not flipping it on a hunch.

### `thumbnail_mesh` was the only shader still using per-bake `vkUpdateDescriptorSets`

A grep + the agent's shader inventory confirmed: every other shader either samples through Set 1 bindless already (PBR) or uses pass-local fixed samplers (GTAO, shadow PCF, postprocess, outline, grid). `thumbnail_mesh` was the lone outlier, and its bake path allocated two private descriptor pools (one for the bake's single set, one for the inspector ring's per-slot sets) just to swap a single texture per render. Migration was strictly net-negative: –120 LOC, identical render output, no per-bake descriptor allocations.

The migration introduced one new concern: the thumbnail bake fetches `VKTexture::GetBindlessIndex()` after a `vkDeviceWaitIdle`. The bindless registration is deferred via the `UploadContext` pump, which drains in `AssetManager::Update`. Between `vkDeviceWaitIdle` and the next `AssetManager::Update` tick, the bindless index can still be `INVALID_BINDLESS_SLOT`. The fix: call `UploadContext::Get().DrainPendingBinds()` immediately after `vkDeviceWaitIdle` to force any ready slots to populate. Cheap (iterates a small list, writes one descriptor per ready entry), idempotent, and bounded by the size of the pending list.

`ResolveBindlessIndex` wraps `BindlessOrNull(tex->GetBindlessIndex())` so even if a drain isn't possible (texture never registered, exotic format, etc.), the shader samples slot 0 (1x1 white) — visually identical to the old `s_WhiteTexture` fallback for material bakes with no diffuse.

---

## Files touched

**Engine (Luth.lib):**
- [`VulkanContext.cpp`](../../../luth/source/luth/renderer/backend/vulkan/VulkanContext.cpp) — `features12.bufferDeviceAddress` + `avail12` validation
- [`VulkanAllocator.cpp`](../../../luth/source/luth/renderer/backend/vulkan/VulkanAllocator.cpp) — `VMA_ALLOCATOR_CREATE_BUFFER_DEVICE_ADDRESS_BIT`
- [`VulkanBuffer.{h,cpp}`](../../../luth/source/luth/renderer/backend/vulkan/VulkanBuffer.h) — `SHADER_DEVICE_ADDRESS_BIT`, cached `m_DeviceAddress`, `GetDeviceAddress()` accessor
- [`GPUTaggedPageAllocator.cpp`](../../../luth/source/luth/memory/GPUTaggedPageAllocator.cpp) — `SHADER_DEVICE_ADDRESS_BIT` on backing + large-one-shot allocations
- [`Material.{h,cpp}`](../../../luth/source/luth/renderer/material/Material.h) — `GPUMaterialData` extended; `UpdateGPUData` resolves 8 maps + repacks flags
- [`VulkanDescriptors.{h,cpp}`](../../../luth/source/luth/renderer/backend/vulkan/VulkanDescriptors.h) — Set 1 binding 1, canonical samplers, `BindSampler`/`UnbindSampler`

**Shaders:**
- [`pbr.frag`](../../../luth/assets/shaders/pbr.frag) — material struct mirror, FLAG_HAS_*, UV_SHIFT_*, bindlessSamplers[] decl
- [`thumbnail_mesh.{vert,frag}`](../../../luth/assets/shaders/thumbnail_mesh.frag) — bindless sampling + diffuseIndex PC

**Editor (Luthien.lib):**
- [`ThumbnailPreviewScene.cpp`](../../../luthien/source/luthien/widgets/ThumbnailPreviewScene.cpp) — bake + inspector paths drop their own descriptor pools / sets

**Docs:**
- [`arch/rendering-pipeline.md`](../arch/rendering-pipeline.md) — Set 1 row + BDA pattern subsection
- [`epics/rt-renderer.md`](../epics/rt-renderer.md) — A.1 Progress Tracker row

---

## Verification

Sub-task A-D / F: C++ builds clean (zero new warnings). Sub-task E: builds clean; runtime smoke-test required for thumbnail visual regression (mesh + material previews) — handed off to the user before the merge.

End-to-end smoke checklist:
1. Editor launches; PBR meshes render byte-identical to pre-effort reference (no validation errors at device create).
2. `bufferDeviceAddress` shows up in any device-create log spew at launch.
3. Project panel: mesh + material thumbnails bake correctly (white-tinted mesh, material's albedo on the sphere).
4. Material inspector: 3D preview renders the sphere with the current material's albedo + color.
5. Frame Debugger: per-draw replay unchanged.
6. RenderDoc capture (optional): Set 1 has two bindings — binding 0 populated as before, binding 1 has the 4 canonical samplers at slots 0-3.
7. Tracy capture (optional): no per-bake `vkAllocateDescriptorSets` / `vkUpdateDescriptorSets` for the thumbnail pipeline.
