# Rendering Pipeline — Architecture Details

## Descriptor Set Allocation

| Set | Content | Updated |
|-----|---------|---------|
| 0 | GlobalUniforms + shadow cascade array + IBL irradiance + IBL prefiltered env + BRDF LUT + GTAO settings UBO + TLAS handle (7 bindings) | Per frame — cycled across `MAX_FRAMES_IN_FLIGHT` slots |
| 1 | b0: Bindless textures (16384 combined image-samplers) + b1: Canonical sampler array (32 slots, 4 reserved at the front: LinearRepeatAnisoMip / LinearClampAnisoMip / NearestRepeatNoMip / NearestClampNoMip) | b0 on upload-fence retire (UPDATE_AFTER_BIND, partially-bound; deferred via `UploadContext` pump per `texture-async-uploads` v2.8.14). b1 fixed-allocated for canonical samplers at startup; `BindSampler`/`UnbindSampler` LIFO over the remaining slots (UPDATE_AFTER_BIND, partially-bound). |
| 2 | Material SSBO (16384 entries) | Per game stage — cycled across `MAX_FRAMES_IN_FLIGHT` slots |
| 3 | LightSSBO (header + flexible PointLightData[], std430) + ClusterGrid SSBO (uvec2 offset+count per cluster) + LightIndex SSBO (flat indices) + shadow map sampler (4 bindings) | Per frame — cycled across `MAX_FRAMES_IN_FLIGHT` slots; **per-view** (cluster grid + index differ between Scene + Game panel views — see `forward-plus` v3.0.2) |
| 4 | `BoneMatrixBuffer` SSBO (per-entity skinning blocks) | Per game stage — cycled across `MAX_FRAMES_IN_FLIGHT` slots |
| 5 | `GPUObjectData` SSBO — per-draw transforms/IDs for indirect dispatch | Per render stage — cycled across `MAX_FRAMES_IN_FLIGHT` slots |
| 6 | Transparent pass-local (`TransparencySubsystem`): b0 fog atlas sampler3D + b1 OIT heads storage image + b2 OIT nodes SSBO | b0 parity-rewritten per frame (UAB, volComposite rule); b1/b2 rewritten on view alloc/resize/budget change (UAB + partially-bound — the sorted pipeline never statically uses them) |

> Set 0 expanded from 4 → 6 bindings across `csm` (v1.3.0 — cascade array) and `gtao` (v1.5.0 — AO sampler + settings UBO). Set 4 added by `animation-gpu-skinning`; Set 5 by `compute-gpu-culling` (v1.2.0). Sets 2/4/5 moved to per-stage rebind in `gpu-tagged-heap` (v2.8.10) — backing storage allocated each frame from `GPUTaggedPageAllocator`, descriptors rewritten via `vkUpdateDescriptorSets`. The cull descriptor (binding into Set 5 + Indirect Buffer for compute) follows the same pattern. Set 1 bindless registration moved from synchronous-in-VKTexture-ctor to a `UploadContext` pending-bind pump in `texture-async-uploads` (v2.8.14) — `VKTexture` ctor pushes `{outIndex, view, sampler, fence}`; pump drains in `AssetManager::Update` once `IsComplete(fence)` and writes the slot through `outIndex`. Until then `m_BindlessIndex == INVALID_BINDLESS_SLOT` and `Material::BindlessOrNull` keeps shaders on reserved white slot 0. `~VKTexture` cancels by view-handle. Set 1 second binding (`rt-renderer.1-bindless`) adds a 32-slot pure-sampler array — canonical samplers (linear/nearest × repeat/clamp) live at fixed slots 0-3 for shader paths that want to pick a sampler independently of the texture's baked one; `BindSampler/UnbindSampler` LIFO-vend the remaining slots for ad-hoc registrations. Today's PBR sampling still rides binding 0's combined image-samplers; `slim-gbuffer` (A.2) and downstream consumers wire actual sampler-by-index usage.

> **Per-frame descriptor cycling (`per-frame-descriptor-set-cycling` v2.9.13).** Every set whose binding is rewritten per stage to a fresh tagged-heap region — Sets 0, 2, 3, 4, 5, the cull descriptor, the 4 PostProcess sets, the GTAO main set, and the Grid set — is now `std::array<VkDescriptorSet, MAX_FRAMES_IN_FLIGHT>`. Render-stage bind sites index by `Renderer::GetFrameData()->GetRenderFrameIndex() % MAX_FRAMES_IN_FLIGHT`; game-stage `Update()` writes the GAME-frame slot (`GetFrameIndex() % N`). Game frame K writes slot K%N while render stage of frame K-1 reads slot (K-1)%N — distinct slots, race-free, no UAB needed. Set 1 (bindless) keeps UAB because of its partial-bind / late-fence-retire pattern. The descriptor-array slot rotation is orthogonal to the heap-region tag (which stays absolute frame index, freed by `FreeTag(N-2)`). The three cross-set co-batched UBO writes (`Global b0 + Grid b0`, `Global b5 + GTAOMain b2`, `4× PP b2`) preserve their atomic-write invariant — both/all writes use the same cached `slot`.

> **Buffer device address (`rt-renderer.1-bindless`).** `bufferDeviceAddress` is enabled in `features12` and propagated to VMA (`VMA_ALLOCATOR_CREATE_BUFFER_DEVICE_ADDRESS_BIT`). Mesh vertex/index buffers and `GPUTaggedPageAllocator` backings carry `VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT`. `VKVertexBuffer::GetDeviceAddress()` / `VKIndexBuffer::GetDeviceAddress()` cache the result of `vkGetBufferDeviceAddress` at ctor time. **No shader path consumes BDA yet** — addresses sit dormant on the buffers. Consumers in later efforts plumb addresses to shaders through one of: extending `GPUObjectData` (Set 5) with two `uint64_t` BDA fields per draw, per-draw push constants, or RT SBTs. The consumer arc picks; A.1 only provisions the foundation.

> **RT extensions + factory classes (`rt-renderer.B.1`).** Four extensions enabled: `VK_KHR_acceleration_structure`, `VK_KHR_ray_tracing_pipeline`, `VK_KHR_ray_query`, `VK_KHR_deferred_host_operations`. Three RT feature structs (`accelerationStructure`, `rayTracingPipeline`, `rayQuery`) enabled + validated at device pick; missing RT means the device fails baseline (RT-mandatory). `VulkanContext::RtFunctions` POD caches 8 PFN_ entry points loaded via `vkGetDeviceProcAddr`. `VulkanContext::GetRtPipelineProperties()` / `GetAsProperties()` cache the RT physical-device properties (handle sizes / alignments / recursion limit). Three new classes: `VKRayTracingPipeline` (RT analog of `VKComputePipeline`, takes `RayTracingStages` POD), `RtShaderBindingTable` (persistent HOST_VISIBLE buffer with 4 canonical regions raygen→miss→hit→callable, alignment math via cached properties), and the `RtSubsystem` lifecycle host.

> **BLAS + TLAS + per-frame skinned refit (`rt-renderer.B.2`).** Per-mesh BLAS built at `Model::ProcessMeshData` for both static and skinned meshes (synchronous main-thread `ImmediateSubmit` on the graphics queue, `PREFER_FAST_TRACE` flag). Skinned meshes additionally allocate per-mesh persistent "skin input" (tight-packed pos + boneIDs + weights, 48 B/vert) and "deformed positions" (12 B/vert) buffers; the source SkinnedVertex VB is kept for raster. New `skinning.comp` compute shader writes deformed positions per frame, reading bone matrices from the existing `BoneMatrixBuffer` SSBO (set 0 binding 0 for the compute pipeline; the binding stageFlags gained `COMPUTE_BIT`). Per-frame TLAS rebuild from `RenderSnapshot::meshes` via `TlasBuildPass` on `QueueFamily::AsyncCompute` (`vkCmdBuildAccelerationStructuresKHR` requires `VK_QUEUE_COMPUTE_BIT` per spec; NVIDIA RTX best-practices recommend async-compute for AS building). Pass body orchestrates: skinning dispatch → compute-write → AS-build-read memory barrier → batched skinned-BLAS refit (one `vkCmdBuildAccelerationStructuresKHR(N, ...)` call with per-mesh scratch sub-regions per NVIDIA's "unique scratch" rule) → AS-build-write → AS-build-read memory barrier → TLAS build with FNV-1a hash dirty short-circuit (skip rebuild when entity+translation set matches the previous frame, keep last TLAS handle alive across frames). Multi-view guard on `RtSubsystem::m_LastBuildFrame` short-circuits the second view (TLAS is scene-global). **TLAS storage + instance buffer per-frame `VulkanAllocator::AllocateBuffer` + `PushDeletion`** (drains N+2 — NOT extending tagged-heap backings); **TLAS + per-mesh-refit scratch via `GPUTaggedPageAllocator::AllocateLargeTagged`** (pure STORAGE_BUFFER consumers, freed N-2). **Set 0 binding 6** added: `VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR` with `UPDATE_AFTER_BIND_BIT | PARTIALLY_BOUND_BIT` — pre-scene null-handle writes are legal per partially-bound when no shader statically reads the binding (B.2 ships none). The per-frame TLAS write is batched into `GlobalSubsystem::UpdateUBO`'s existing cross-set co-batched call (`writes[2]` → `writes[3]`). `ResourceState::AccelerationStructure{Build,Read}` enum entries + `GetStateInfo` arms added so B.3's RT shadow consumer can use the RG barrier solver natively. **Set 6 RT-output/SBT layout deferred to B.3** when its first RT shader writes outputs.

> **ReSTIR DI direct lighting (`rt-renderer.C.1`).** Four compute passes on `QueueFamily::AsyncCompute`, between the TLAS build and `GeometryPass`: `RestirInitial` (RIS over the flat point-light list + one `rayQueryEXT` visibility ray on the selected sample — first ray-query-in-compute; **AS-build→read barrier dst = `COMPUTE_SHADER_BIT`**, not the `RAY_TRACING_SHADER_BIT` the B.3 SBT pass uses) → `RestirTemporal` (motion-vector reprojection + linear-depth/normal validation + confidence-weighted combine, Bitterli relative M-cap `prev.M ≤ mCap·curr.M`) → `RestirSpatial` (k-neighbour disk merge into a separate output — neighbours read un-modified, never in-place; no Jacobian since punctual lights store a light index) → `RestirShade` (demodulated diffuse irradiance `E = Li·NdotL·W`). Pass-local **Set 2** (b0/b1 depth+normal samplers, b2/b4 curr/prev reservoir SSBOs UAB-swapped by `frameAbs&1`, b3 DI storage image, b5 motion sampler, b6 spatial output). The reservoir **ping-pong pair** + spatial-output buffer are **device-local Garlic** (`GPUTaggedPageAllocator::AllocateLargeTaggedDeviceLocal`, persistent per-view, reserved `0xFFFF0000+` tags freed only on resize — S0). The reservoir self-carries its pixel geometry in the struct's 3 `_pad` floats, so temporal validation needs no previous-frame G-buffer. `pbr.frag` Set 3 b5 samples the DI image and remodulates `albedo·(1-metallic)/π`, gated on `GlobalUniforms::restirParams.x`; the unshadowed cluster loop is the A/B fallback. Tuning via `RestirSettings` (RenderPanel). **C.2 SVGF** slots between `RestirShade` and `GeometryPass`, denoising the demodulated signal before remodulation.

> **SVGF denoiser (`rt-renderer.C.2`).** Schied 2017 SVGF over the demodulated DI, between `RestirShade` and `GeometryPass`, all `AsyncCompute`: `svgf_reproject` (motion reproject + 2×2 bilinear disocclusion via relative linear-depth + normal dot — compute has no `fwidth` — + EMA color/luminance-moments) → `svgf_moments` (temporal variance, or 7×7 spatial bilateral for `histLen<4` ×`4/histLen`) → `svgf_atrous ×N` (5×5 B-spline edge-aware; depth-gradient/normal/luma edge-stops; variance co-filtered with **squared** weights; the final level writes the consumed image). Per-view history is **RGBA16F images, not Garlic buffers** (à-trous + reprojection need bilinear taps): `colorHist`/`moments`/`geom` cross-frame ping-pong + `svgfAtrous` within-frame ping-pong + `svgfDenoised`, all kept **GENERAL** + bootstrap-cleared and threaded through the RG via the new **`ReadStorageImageGeneral`** (a storage read that stays GENERAL across passes — `ResourceState::ComputeReadStorage`; plain `ReadStorageImage` transitions to SHADER_READ_ONLY, which mismatches a storage `imageLoad`). `pbr.frag` Set 3 b5 binds `svgfDenoised` (the denoiser owns the slot whenever ReSTIR is on; disabled = raw passthrough). **feedbackTap = −1** (reproject's integrated color is the temporal feedback; à-trous is a pure post-filter) — the research-preferred feedbackTap = 1 + A-SVGF (Schied 2018) deferred. Wrapped in an `IDenoiser` interface (NRD/RELAX `RELAX_DIFFUSE` swap reserved). f16 moment-overflow clamped (luminance ≤ 255) + finite-guarded. Tuning via `SvgfSettings` (RenderPanel).

> **ReSTIR GI indirect diffuse (`rt-renderer.C.3`).** Ouyang 2021 — 1-bounce indirect-diffuse via per-pixel *path* reservoirs, four `AsyncCompute` passes after the TLAS build, sibling to C.1: `GiInitial` (cosine-hemisphere bounce → rayQuery **commit-hit** → secondary-hit `L_o` → 64 B `GIReservoir`) → `GiTemporal` → `GiSpatial` → `GiShade` (demodulated `E = L_o·NdotL·W`). Reuse carries a **reconnection Jacobian** (Ouyang Eq. 11; crossed cos/dist² ratios, both cosines at the sample point, reject [1/10,10] → clamp [1/3,3]) — the piece C.1 skipped (light-index reservoirs → identity shift). Spatial uses **RTXDI BASIC** `pi/piSum` bias correction. Device-local Garlic reservoir ping-pong + spatial buffer, reserved tags from `0xFFFF8000` (disjoint from DI's `0xFFFF0000`); the reservoir self-carries receiver pos+packed-normal (the packed normal MUST be a `uint` SSBO field via `packHalf2x16` — a `float` round-trip canonicalizes denormals). **Cross-frame reservoir reads import `prev` as `StorageBufferWrite`, not `Undefined`** — else the read barrier carries `srcAccess=0`, no availability op, and temporal never accumulates (the async-compute queue is chained frame-to-frame). **Real secondary-hit material (S3):** `instanceCustomIndex` is repurposed (was the unused entity id) to index a per-frame **geometry table** built in `TlasBuilder` in lockstep with the packed instances (`{vertexBDA, indexBDA, materialSlot, vertexStride}`, 24 B); `restir_gi_initial.comp` deref's the hit triangle via `GL_EXT_buffer_reference2` → barycentric UV + world-space geometric normal → bindless Material SSBO (set 3) + textures (set 4). The table's BDA rides the `GiPC` push constant, read at preflight from the same `m_LastResult` that binds Set 0 b6, so the table can never desync from the bound TLAS; `materialUUID` is folded into the TLAS rebuild hash so a runtime material swap on a static mesh forces a rebuild. **Denoise (S4):** a second channel-parameterized `SvgfDenoiser` instance (`DenoiserChannel::Gi`) over the C.2 chain, driving flat-parallel `svgfGi*` ViewResources history + a separate `SvgfGiSettings`; `pbr.frag` Set 3 **b6** binds `svgfGiDenoised` (the GI denoiser owns the slot, mirroring DI's b5) and adds `E·albedo·(1-metallic)/π` under `restirParams.y`. Tuning via `RestirGiSettings` + `SvgfGiSettings` (RenderPanel). GTAO modulates only the IBL ambient term, never the RT GI (no double-darken).

> **Path-traced reference mode (`rt-renderer.C.5`).** A top-level `RenderMode::PathTrace` toggle (distinct from the `ShadeMode` debug-blit enum) swaps the raster + ReSTIR chain for a single rayQuery-in-compute **megakernel** (`path_trace.comp`, `PathTraceSubsystem`, AsyncCompute after the TLAS build). It traces its OWN jittered primary camera rays (no G-buffer dependency → free progressive AA), walks a multi-bounce NEE path — full Cook-Torrance BRDF matching `pbr.frag` (D/G/F + kD), point+sun NEE, emission, IBL prefiltered-env on miss — with cosine-diffuse + GGX VNDF specular sampling under **one-sample lobe MIS**, and Russian-roulette termination. The reference accumulates an fp32 running mean (`ptAccum`, **in-place RMW**, cross-frame RAW via the `ComputeWrite` import + `ReadStorageImageGeneral`/`WriteStorageImage` — the GI-reservoir cross-frame pattern) reset on a per-view FNV hash of camera VP + scene instances + lights + exposure + settings + a manual salt. The fp16 display copy (`ptColor`) replaces `sceneColor` ahead of bloom/tonemap via the `UpdateBloomCompositeInput` PT branch; the raster chain dead-pass-culls. **TAA + scene overlays gated off** in PT (the projection jitter is held static so the accumulation doesn't restart every frame). New `common/geom_table.glsl` shares the hit-surface deref — smooth `s.ns` (barycentric vertex normals, matches raster `v_Normal`) for shading, geometric `s.ng` for ray-origin offsets — with `restir_gi_initial.comp` (which keeps `s.ng`). MIS is degenerate across the engine's disjoint light pathways (punctual NEE / emissive-on-hit / env-on-miss) — the genuine MIS is the diffuse↔specular lobe pdf; the BRDF deliberately matches `pbr.frag`'s approximate Smith-G (A/B isolates light transport) while the VNDF pdf uses the analytic Smith G1 (unbiased). The whole RT path is opaque-only (cutout/transparent treated opaque — `gl_RayFlagsOpaqueEXT` everywhere). Tuning + convergence readout via `PathTraceSettings` (RenderPanel).

> **Subsystem ownership (`render-pipeline-subsystems`).** Each Vulkan descriptor Set's full lifecycle (layout + pool + per-view set + binding writes + per-frame upload) lives in one subsystem under `luth/source/luth/renderer/subsystems/`:
>
> | Set / domain | Subsystem |
> |---|---|
> | Set 0 (Global) | `GlobalSubsystem` (UpdateUBO writes binding 0 + Grid binding 0 atomically — both share the same heap region) |
> | Set 3 (Lighting + shadow) | `LightingSubsystem` (also owns IBL + shadow map + skybox) |
> | Set 5 (GPUObjectData) + cull descriptor | `GeometrySubsystem` (also owns PBR + DepthPrepass pipelines + entity↔SSBO maps) |
> | GTAO compute (3 layouts) | `GTAOSubsystem` (UpdateUBO writes Set 0 binding 5 + GTAOMain binding 2 atomically) |
> | PostProcess (4 sets, shared layout) | `PostProcessSubsystem` (bloom + tonemap pipelines, `UpdateUBO` rebinds binding 2 of all 4 sets) |
> | Outline + Grid | `EditorOverlaysSubsystem` (also owns SelectionMask pipelines + 3 `Add*Pass`) |
>
> Sets 1 (bindless), 2 (Material), 4 (BoneMatrixBuffer) live outside the subsystem split — owned by their respective scene-side systems. `RenderPipeline` is now a ~650-LOC orchestrator: holds the 6 subsystem instances, dispatches `Init`/`Shutdown`/`Update`/`Add*Pass` in dependency order, owns frame-scratch state (`m_CurrentView`, `m_CurrentViewResources`, `m_ViewResources` map), `AddImGuiPass`, named-texture registry, shader-reload dispatcher, and frame-debugger forwarders. Friend declarations between `RenderPipeline` ↔ `RenderingSystem` and `RenderPipeline` ↔ `FrameDebuggerContext` fully removed.

## Current RenderGraph Pass Order

```
(per shadow cascade × 4)
CullComputePass (shadow) ─┐
ShadowPass (depth-only)   ─┘

DepthPrepass (depth-only, main view, indirect draw)
  ↓
SlimGBufferPass (normal RG16F + roughness R8 + motion RG16F + matID R16U; opaque reads prepass depth via EQUAL test; cutout alpha-tests + writes its own depth via LESS_OR_EQUAL so RT shadows/reflections + GTAO reconstruct from the holed surface)
  ↓
ClusterBuildPass (compute, AsyncCompute) — per-cluster view-space AABB via Olsson log depth slicing
  ↓
LightAssignPass (compute, AsyncCompute) — sphere-vs-AABB per cluster, atomic-packs LightIndex + writes ClusterGrid (offset, count)
  ↓
GTAO: PrefilterPass → MainPass → DenoisePass (all on AsyncCompute, sequential after cluster passes on the same compute primary)
  ↓
CullComputePass (main scene) — populates per-draw indirect args
  ↓
GeometryPass (PBR forward — opaque/cutout only; reads prepass depth + AO + shadows + LightSSBO/ClusterGrid/LightIndex via per-view Set 3; specular AA inline via dFdx(N)/dFdy(N) curvature variance)
  ↓
SelectionMaskPass (entity-ID → mask for outline)
  ↓
SkyboxPass (depth = 1.0 trick, HDR)
  ↓
VolumetricCompositePass (alpha-blends fog into HDR sceneColor)
  ↓
Transparent tier (TransparencySubsystem; skipped when the transparent bucket is empty or in PT mode):
  Sorted mode — TransparentPass (per-view back-to-front indirect draws, depth-test-no-write, alpha blend)
  OIT mode    — OITClear (heads → OIT_EMPTY + node-count → 0) → OITStore (depth-only raster; shades once,
                pushes PPLL nodes via Set 6 atomics) → OITResolve (fullscreen sort-K + ONE/SRC_ALPHA composite)
  Both shade via pbr_transparent_shading.glsl: rayQuery sun shadow (geom_table candidate loop) / CSM by
  ShadowingMode, cluster point-light loop, IBL ambient, froxel fog at the FRAGMENT's depth; never the
  opaque-coupled screen-space inputs (RT mask / ReSTIR DI / GI / GTAO). Writes EntityID (picking parity).
  ↓
TaaResolvePass (Karis14 YCoCg-clip — reads HDR sceneColor + slim G-buffer motion + parity-picked taaHistoryPrev; writes parity-picked taaHistoryCurr. Output flows through grid + bloom + composite.)
  ↓
GridPass (optional, editor-only overlay — writes on TAA output in-place)
  ↓
BloomExtractPass → BloomBlurH/V (separable 9-tap Gaussian, half-res; reads TAA-resolved color so bloom blooms anti-aliased HDR)
  ↓
PostProcessPass (tonemap [ACES / Uncharted 2 / AgX / AgX Punchy] + bloom compose + vignette + grain + CA → LDR)
  ↓
SlimVizPass / ClusterVizPass (conditional — gated by ShadeMode; SlimViz for Slim*, ClusterViz for ClustersDensity (cluster_viz.frag samples SceneDepth → 3D cluster ID → heat-map); blit over LDR)
  ↓
OutlinePass (reads mask + depth, composites onto LDR)
  ↓
ImGuiPass (composites editor UI onto LDR → swapchain)
```

Pass invocations live in `RenderPipeline.cpp::BuildGraph` (chain visible at lines ~400–442). All passes go through the render graph for barrier insertion + dead-pass culling.

## Target RenderGraph Pass Order (post-`rt-renderer` arc)

End state targeted by the `rt-renderer` series (v3.0.0). RT-mandatory; the raster CSM path retires in Phase B.3.

```
DepthPrepass (depth-only, main view, indirect draw)
  ↓
CullComputePass (main scene) — populates per-draw indirect args
  ↓
SlimGBufferPass (normal RG16F + roughness R8 + motion vectors RG16F + material ID R16U; reads prepass depth)
  ↓
GTAO chain (PrefilterPass → MainPass → DenoisePass) — half-res AO; supplements RT GI's slow indirect convergence
  ↓
ClusterBuildPass (compute, async-compute eligible)
  ↓
LightAssignPass (compute, async-compute eligible)
  ↓
VolumetricInjectPass (compute) — voxel density + in-scattering from cluster lights + RT shadow rays per cell
  ↓
VolumetricIntegratePass (compute) — accumulate along view rays into froxel volume
  ↓
TlasBuildPass (per-frame for dynamic instances; static skipped)
  ↓
RtDirectLightingPass (ReSTIR DI — reservoir-sampled visibility handles both light selection and shadow)
  ↓
RtGiPass (ReSTIR GI — indirect bounce reservoirs)
  ↓
SvgfDenoisePass (spatial + temporal accumulation for RT outputs)
  ↓
GeometryPass (forward, clustered light loop, reads RT direct + GI, samples volumetric)
  ↓
SkyboxPass (depth = 1.0 trick, HDR)
  ↓
TransparentPass — SHIPPED as the transparency tier (v3.1.3): runs after the volumetric composite,
Sorted or PPLL-OIT (see the current-order listing above); shadow-ray-excluded via TLAS cull masks,
visible to GI/reflections/PT as a committed surface
  ↓
RtReflectionsPass (stochastic ray dispatch + dedicated denoise)
  ↓
GpuParticleSimPass (compute) → ParticleRenderPass (alpha-blended sprites)
  ↓
SelectionMaskPass (entity-ID → mask for outline)
  ↓
TaaResolvePass (Karis14 — reads motion vectors from slim G-buffer, reads history)
  ↓
BloomExtractPass → BloomBlurH/V (separable 9-tap, half-res)
  ↓
PostProcessPass (tonemap [ACES variant / AgX] + bloom compose + vignette + grain + CA → LDR)
  ↓
OutlinePass (reads mask + depth, composites onto LDR)
  ↓
GridPass (editor overlay)
  ↓
ImGuiPass (composites editor UI onto LDR → swapchain)
```

> Pass-order details may shift per phase landing. Phase B.3 may ship a standalone `RtSunShadowsPass` that consolidates into ReSTIR DI when Phase C.1 lands — both shapes are correct. Phase C.1/C.3 integration may merge `RtDirectLightingPass`/`RtGiPass` into a single resampling pass.

## Active modernization — `rt-renderer` arc (v3.0.0)

The arc layers new pass families onto the existing render graph; foundational systems (job system, render graph DAG, frame pipeline, memory primitives, queue topology) stay unchanged. Key data-structure additions by phase:

**Phase A — modern foundation**
- Set 1 (bindless) scales up; new bindless sampler array + bindless mesh access via `VK_KHR_buffer_device_address` (v3.0.0 — `bindless-migration`)
- Slim G-buffer attachments (normal RG16F oct + roughness R8 + motion RG16F NDC + matID R16U) — feeds TAA + RT denoising (v3.0.1 — `slim-gbuffer`). `GlobalUniforms` + `prevViewProjection`, `GPUObjectData` + `prevModel` + `prevBoneOffset`, `BoneMatrixBuffer` dual-region for prev-frame bones. Per-view `prevViewProj` storage on `ViewResources` (not `GlobalSubsystem` — single global cross-contaminated under multi-view rendering)
- Set 3 reshapes: fixed `LightUBO` → unbounded `Light SSBO` + `Cluster grid SSBO` + `Light index SSBO`, all from `GPUTaggedPageAllocator`
- Volumetric voxel volume (compute storage image, frustum-aligned)
- Image quality (v3.0.7 — `image-quality`): Halton(2,3) prefix-8 jitter on projection (per-view state on `ViewResources` matching the `prevViewProj` precedent — multi-view contamination hazard); per-view RGBA16F `taaHistoryA/B` ping-pong via `frameAbs` parity; `TaaResolvePass` runs HDR-domain between volumetric composite and bloom (Karis14 YCoCg-clip recipe lifted from playdead/temporal MIT); Tokuyoshi19 specular AA inline in `pbr.frag`; AgX tonemap operator gains two enum slots (`AgX`, `AgXPunchy`) — output contract is linear sRGB, tail gamma in `postprocess.frag` is load-bearing

**Phase B — RT foundation**
- New descriptor set for RT (TLAS binding + RT-output storage images + SBT)
- BLAS per mesh asset (built on import or geometry change); TLAS rebuilt per frame from scene instance list
- Raster shadow pipeline (ShadowPass + cascade selection in `LightingSubsystem`) retires when B.3 lands

**Phase C — RT GI**
- ReSTIR reservoir buffers (per-pixel ping-pong, `GPUTaggedPageAllocator`)
- SVGF history buffers (color + moments, ping-pong) behind an `IDenoiser` abstraction for later NRD swap

**Phase C.5 — PT reference mode**
- Parallel `PathTraceMode` toggle; reuses Phase B BLAS/TLAS + Phase C material BRDF sampling; persistent accumulation image; bypasses raster + ReSTIR

**Phase D — reflections + atmospheric polish**
- RT reflections (stochastic samples + denoise outputs)
- Volumetric voxel volume gains RT shadow-ray writes per cell
- GPU particle simulation/render passes added downstream of GeometryPass

Full per-phase work in `docs/development/epics/rt-renderer.md` (local spec, never committed).

### Render-graph hazards (documented from slim-gbuffer + forward-plus + rg-depth-handoff smoke)

Four RG patterns surfaced during recent renderer work — worth knowing before adding new passes:

- **Re-importing a VkImage that another pass in the same frame already imported aliases it onto two `ResourceNode`s**. Each `rg.ImportResource(...)` creates a new node with its own `currentState`/`lastWriter`. The barrier solver tracks state per-node, so two nodes wrapping the same VkImage diverge: one node thinks the resource is in state X (from the producer's `Write`), the other in state Y (from the consumer's `ImportResource(... initialState)`). The consumer-side barrier's `oldLayout` mismatches the actual GPU layout → VUID 01197. **Pattern:** when a downstream pass writes/reads a target that an upstream pass produced this frame, take the producer's returned handle as a parameter and use `builder.Read(handle)` / `builder.Write(handle)` — same node, consistent state tracking.
- **Per-view render state stored on a per-pipeline subsystem causes multi-view contamination.** A single `Mat4` on a subsystem (e.g., `GlobalSubsystem::m_CachedViewProj`) is fine when only one view renders per frame, but breaks the moment another view's `RecordView` runs in the same frame — the second view's read sees the first view's write, not its own previous value. **Pattern:** per-view state lives on `ViewResources` (or any per-view container). `m_CachedViewProj` still works for its existing role (frustum cull within one view's render) because write + read alternate per-view; cross-frame caching does not.
- **`BufferHandle` is for barrier tracking, not descriptor binding** (`forward-plus` smoke). `RG::BufferHandle` carries `index + version` of an internal `BufferNode`; the node stores only the backing `VkBuffer` pointer (not the sub-region offset within that backing). For tagged-heap-backed buffers, multiple `ImportBuffer` calls on the same `VkBuffer` at different offsets create multiple nodes that all map to the same physical buffer. Resolving a downstream pass's binding via `rg.GetBuffers()[handle.index - 1]` only gives you the `VkBuffer`; the offset must come from the producer's `GPUSubRegion`. **Pattern:** producer returns its `BufferHandle` *plus* `GPUSubRegion(s)` in the output struct; consumer's `VkDescriptorBufferInfo` uses `subRegion.buffer + subRegion.offset + subRegion.size`. Existing `GeometrySubsystem::BuildGPUObjectBuffer` already follows this — internal RG sites do `m_Buffers[handle.index - 1]` only for barrier-solve bookkeeping, never for binding offsets.
- **Image barriers must set `srcQueueFamilyIndex = dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED` explicitly, not rely on zero-init** (`rg-depth-handoff` smoke). A zero-initialized `VkImageMemoryBarrier2` leaves both fields at 0 (= the graphics family index on most hardware). For images created with `VK_SHARING_MODE_CONCURRENT` (the policy at `arch/multi-queue.md`), the spec requires both indices to be `VK_QUEUE_FAMILY_IGNORED` per `VUID-VkImageMemoryBarrier2-image-04071`. Passing 0/0 violates the spec; the observed symptom was `VUID-vkCmdDraw-None-09600` firing several passes downstream at a descriptor-sampling draw, with the solver trace confirming every prior barrier was decided and emitted as expected (so the issue is at emission, not state tracking). **Pattern:** every image-barrier emission site sets both indices to `VK_QUEUE_FAMILY_IGNORED` explicitly. Buffer barriers in `RenderGraph::Execute` already do this; image barriers must match.

> **Transparency tier (`transparency-tier` v3.1.3).** Transparent/Fade is its own pass slot after the
> volumetric composite — GeometryPass draws opaque + cutout only. Two runtime modes
> (`TransparencySettings::mode`): **Sorted** (per-view back-to-front over the shared bucket via a
> LinearAllocator index array — indirect slots are keyed by `gpuObjectIndex`, so sorted ITERATION alone
> reorders submission) and **PPLL OIT** (default): per-pixel linked list — R32_Uint heads image
> (per-view, `ViewResources`) + a Garlic `AllocateLargeTaggedDeviceLocal` node pool (16 B nodes
> `{colorRGB9E5, depth24|alpha8, entityID, next}`, reserved tags `0xFFFFC000+`, freed on
> resize/budget/release), cleared per frame, resolved by a fullscreen K-nearest insertion sort with
> order-independent tail merge, composited `src=ONE dst=SRC_ALPHA`. Transparent shading deliberately
> consumes NO opaque-depth-coupled screen-space input — sun shadow is a per-fragment alpha-tested
> rayQuery (the `geom_table.glsl` seam; CSM branch in CSM mode), point lights via the cluster loop,
> fog via the froxel atlas at the fragment's depth (`common/froxel.glsl`). **RT visibility is
> per-ray-class via TLAS instance cull masks** (`GT_VIS_SOLID 0x01` / `GT_VIS_ALL 0x03` in
> geom_table.glsl, mirrored by TlasBuilder): transparent instances pack with mask GLASS (0x02) +
> FORCE_OPAQUE — shadow-class rays (sun/light visibility, fog shadows) cull to SOLID so glass never
> blocks light, while world-class rays (GI bounce / reflections / PT) trace ALL and commit glass as
> an unblended surface (emissive glass feeds the GI bounce, glass shows in reflections). Glass is
> also dropped from the CSM shadow batches. Blended/refractive RT transparency = future follow-up.
> **New RG states:** `FragmentStorageRead/Write` (GENERAL + FRAGMENT stage; write carries
> SHADER_READ for RMW atomics) — the `Compute*` states emit COMPUTE-stage barriers and would
> under-synchronize fragment-stage storage producers/consumers. **Cross-frame import rule:** the
> heads image + node pool are reused across frames, so OITClear imports them in their END-of-frame
> state (`FragmentStorageRead`) — an `Undefined` import carries srcStage TOP and lets the clear race
> the previous frame's resolve reads (WAR); the heads image is bootstrap-transitioned to GENERAL at
> creation so the frame-0 claim holds.

### Cross-pass numerical contracts

Pass splits where one pass precomputes a value the next pass integrates must publish their numerical contract somewhere both shaders reference. RG state tracking only catches *layout* drift; it does nothing about *math* drift (units, missing factors, double-application). The pattern that bit us:

- **Volumetric inject/integrate contract** — inject_scatter writes `scat = albedo × J` where `J = Σ_lights phase × L × visibility + ambient`. Integrate accumulates `transmit × scat × (1 − exp(−σ_t · dt))`. Together they reconstruct `σ_s × J × (1 − exp(−σ_t · dt)) / σ_t` for `albedo = σ_s/σ_t` (the canonical Wronski 2014 / Hillaire 2015 formulation). The inject **must not** pre-multiply by density — that's the σ_t factor integrate's `(1 − exp)` already encodes. Pre-multiplying double-applies σ_t and dims fog by `density` everywhere (≈ 10× at density 0.1; invisible at density 1.0 because × 1 is a no-op, which is why pre-unified FogVolume usage hid the deviation). The contract lives as a `// CONTRACT:` block at the top of both shader files; any new contributor reading them sees the math agreement explicitly.

**Pattern for future split passes:** when shader A produces a value shader B integrates, write a one-paragraph contract at the top of *both* files, citing the source equation. If the producer's storage layout changes (e.g., packing tint into atlas .gba), update both contracts in lockstep. Reviewer's checklist for any inject/integrate-style split: "are the units in A and B documented as a matching pair?"

## Memory Budget

Indicative at 1920×1080, single view. The per-view RT / denoiser / transparency buffers below dominate
VRAM and multiply by active view count (Scene + Game panels).

| Buffer | Size |
|--------|------|
| Material SSBO | 16384 × 80B = 1.25 MB |
| Light SSBO + Cluster grid + Light index (Set 3) | per-view, dynamic from `GPUTaggedPageAllocator` (replaced the 64-light UBO in `forward-plus`) |
| Global UBO | view/proj + prevViewProj + IBL/GTAO/RT/restir params ≈ <1 KB |
| CSM shadow array | 2048² × D32 × 4 cascades = 64 MB (raster path; RT sun-shadow R8 mask is default) |
| Slim G-buffer | normal RG16F + rough R8 + motion RG16F + matID R16U ≈ 23 MB |
| HDR sceneColor | RGBA16F ≈ 16.6 MB |
| Volumetric froxel volume | 160×90×128 RGBA16F ≈ 14 MB/atlas, per view |
| TAA history | RGBA16F A/B ping-pong, per view ≈ 33 MB |
| OIT (PPLL) | heads R32U 8.3 MB + node pool ≈ 133 MB at `avgLayersBudget` 4, per view |
| ReSTIR DI/GI reservoirs | device-local Garlic ping-pong + spatial (GI reservoir 64 B/px), per view |
| SVGF history | color/moments/geom + à-trous ping-pong RGBA16F, ×2 channels (DI+GI), per view |
| BoneMatrixBuffer | dual-region (curr + prev-frame bones for skinned motion), per skinned entity |
| IBL | irradiance + prefiltered env (5 mips) + BRDF LUT 512² × 8B = 2 MB |
