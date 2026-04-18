#include "luthpch.h"
#include "luth/scene/systems/RenderingSystem.h"
#include "luth/renderer/RenderPipeline.h"
#include "luth/jobs/JobSystem.h"
#include "luth/core/Profiler.h"
#include "luth/scene/Scene.h"
#include "luth/scene/Components.h"
#include "luth/renderer/Renderer.h"
#include "luth/renderer/material/MaterialSystem.h"
#include "luth/renderer/BoneMatrixBuffer.h"
#include "luth/renderer/backend/vulkan/VulkanBackend.h"
#include "luth/renderer/backend/vulkan/VulkanContext.h"
#include "luth/renderer/backend/vulkan/VulkanTexture.h"
#include "luth/renderer/backend/vulkan/VulkanBuffer.h"
#include "luth/renderer/material/Material.h"
#include "luth/renderer/resources/Model.h"
#include "luth/resources/AssetManager.h"
#include "luth/renderer/resources/Buffer.h"
#include "luth/resources/FileSystem.h"
#include "luth/resources/AssetDatabase.h"
#include "luth/renderer/shader/ShaderCompiler.h"
#include "luth/renderer/lighting/IBLPrecompute.h"
#include "luth/renderer/draw/DrawCommand.h"
#include "luth/renderer/passes/CullPass.h"
#include "luth/renderer/backend/vulkan/VulkanAllocator.h"
#include "luth/renderer/shader/ShaderLibrary.h"
#include "luth/renderer/backend/vulkan/VulkanShader.h"
#include "luth/renderer/backend/vulkan/DynamicRendering.h"
#include <backends/imgui_impl_vulkan.h>
#include <imgui.h>
#include <glm/gtc/matrix_transform.hpp>
#include <vma/vk_mem_alloc.h>

namespace Luth
{
    using namespace Component;

    // =========================================================================
    // Construction / Destruction
    // =========================================================================

    RenderingSystem::RenderingSystem(u32 viewportWidth, u32 viewportHeight)
    {
        m_FrameAllocator = std::make_unique<Memory::LinearAllocator>(1 * Memory::MB);
        m_Pipeline       = std::make_unique<RenderPipeline>(*this);
        m_Pipeline->Initialize(viewportWidth, viewportHeight);
    }

    RenderingSystem::~RenderingSystem()
    {
        m_Pipeline->Shutdown();
    }

    void RenderingSystem::ReloadSkybox(const fs::path& hdrPath)
    {
        m_Pipeline->ReloadSkybox(hdrPath);
    }

    // =========================================================================
    // Project lifecycle
    // =========================================================================

    void RenderingSystem::OnProjectLoaded()
    {
        // Add the project's shaders dir (if it exists) to the hot-reload watcher
        // alongside the engine shader dir registered in the constructor.
        if (!FileSystem::HasProject()) return;

        fs::path projectShaders = FileSystem::AssetsPath("shaders");
        if (!fs::exists(projectShaders) || !fs::is_directory(projectShaders))
            return;

        m_ShaderWatcher.AddWatch(projectShaders);
        m_WatchedProjectShaderDir = projectShaders;
        LH_CORE_INFO("Shader hot-reload watching project dir: {}", projectShaders.string());
    }

    void RenderingSystem::OnProjectUnloaded()
    {
        if (m_WatchedProjectShaderDir.empty()) return;
        m_ShaderWatcher.RemoveWatch(m_WatchedProjectShaderDir);
        m_WatchedProjectShaderDir.clear();
    }

    void RenderingSystem::UpdatePostProcessUBO()
    {
        PostProcessUBO ubo{};
        ubo.bloomThreshold      = m_PostProcessSettings.bloomThreshold;
        ubo.bloomStrength       = m_PostProcessSettings.bloomStrength;
        ubo.exposure            = m_PostProcessSettings.exposure;
        ubo.contrast            = m_PostProcessSettings.contrast;
        ubo.saturation          = m_PostProcessSettings.saturation;
        ubo.tonemapOp           = static_cast<int>(m_PostProcessSettings.tonemapOp);
        ubo.vignetteAmount      = m_PostProcessSettings.vignetteAmount;
        ubo.vignetteHardness    = m_PostProcessSettings.vignetteHardness;
        ubo.grainAmount         = m_PostProcessSettings.grainAmount;
        ubo.sharpness           = m_PostProcessSettings.sharpness;
        ubo.chromaticAberration = m_PostProcessSettings.chromaticAberration;
        ubo.time                = Time::GetTime();
        ubo.shadowBalance       = m_PostProcessSettings.shadowBalance;
        ubo.midtoneBalance      = m_PostProcessSettings.midtoneBalance;
        ubo.highlightBalance    = m_PostProcessSettings.highlightBalance;
        m_PostProcessUBOBuffer->SetData(&ubo, sizeof(PostProcessUBO));
    }

    // =========================================================================
    // IBL precomputation
    // =========================================================================

    u32 RenderingSystem::EnsureMaterialRegistered(std::shared_ptr<Material> material)
    {
        auto it = m_MaterialSlotMap.find(material->Handle);
        if (it != m_MaterialSlotMap.end())
            return it->second;

        u32 slot = MaterialSystem::RegisterMaterial(material);
        m_MaterialSlotMap[material->Handle] = slot;
        return slot;
    }

    // =========================================================================
    // Per-frame updates
    // =========================================================================

    void RenderingSystem::UpdateLightUniforms(Scene* scene)
    {
        LightUniforms lights{};
        m_LightGatherer.Gather(scene->Registry(), lights, m_ShadowParams);
        m_LightUniformBuffer->SetData(&lights, sizeof(LightUniforms));

        m_CascadeBuilder.Build(lights.dirLight.direction, m_CameraParams, m_ShadowParams, m_Cascades);
    }

    // =========================================================================
    // GPU Object Buffer + Cull Pipeline
    // =========================================================================

    void RenderingSystem::UpdateGTAOUBO()
    {
        if (!m_GTAOUBOBuffer) return;

        const auto& s = m_PostProcessSettings.gtao;
        GTAOUBO ubo{};
        ubo.intensity      = s.intensity;
        ubo.radius         = s.radius;
        ubo.falloff        = s.falloff;
        ubo.power          = s.power;
        ubo.sliceCount     = s.sliceCount;
        ubo.stepsPerSlice  = s.stepsPerSlice;
        ubo.enabled        = s.enabled  ? 1 : 0;
        ubo.visualize      = s.visualize ? 1 : 0;

        const u32 halfW = m_GTAOLinearDepth ? m_GTAOLinearDepth->GetWidth()  : 1u;
        const u32 halfH = m_GTAOLinearDepth ? m_GTAOLinearDepth->GetHeight() : 1u;
        const u32 fullW = m_Targets.GetSceneColor() ? m_Targets.GetSceneColor()->GetWidth()  : 1u;
        const u32 fullH = m_Targets.GetSceneColor() ? m_Targets.GetSceneColor()->GetHeight() : 1u;
        ubo.invResolution[0]     = 1.0f / float(halfW);
        ubo.invResolution[1]     = 1.0f / float(halfH);
        ubo.invFullResolution[0] = 1.0f / float(fullW);
        ubo.invFullResolution[1] = 1.0f / float(fullH);

        m_GTAOUBOBuffer->SetData(&ubo, sizeof(GTAOUBO));
    }

    void RenderingSystem::BuildGPUObjectBuffer(entt::registry& registry)
    {
        auto* objectData   = static_cast<GPUObjectData*>(m_ObjectSSBOMapped);
        auto* indirectCmds = static_cast<VkDrawIndexedIndirectCommand*>(m_IndirectBufferMapped);
        u32   count        = 0;

        // Rebuild entity lookup table here (consumed by GeometryPass + mouse picking)
        // index 0 = null sentinel; valid entities start at index 1
        m_EntityLookup.clear();
        m_EntityLookup.push_back(entt::null);
        m_EntityToSSBOIndex.clear();

        auto view = registry.view<WorldTransform, MeshRenderer>();
        for (auto [entity, wt, mr] : view.each())
        {
            if (count >= k_MaxGPUObjects) break;
            if (!mr.ModelUUID.IsValid()) continue;

            auto model = AssetManager::GetAsset<Model>(mr.ModelUUID);
            if (!model) continue;
            const auto& meshesData = model->GetMeshesData();
            if (mr.MeshIndex >= (u32)meshesData.size()) continue;
            auto mesh = model->GetMesh(mr.MeshIndex);
            if (!mesh) continue;

            GPUObjectData& obj = objectData[count];
            obj.model = wt.Matrix;

            // Bounding sphere from BindPoseAABB (local space)
            const auto& aabb   = meshesData[mr.MeshIndex].BindPoseAABB;
            obj.boundingSphere = glm::vec4(aabb.Center(), glm::length(aabb.Extents()));

            // Material slot
            u32 matSlot = 0;
            if (mr.MaterialUUID.IsValid()) {
                auto it = m_MaterialSlotMap.find(mr.MaterialUUID);
                if (it != m_MaterialSlotMap.end()) matSlot = it->second;
            }
            obj.materialIndex = matSlot;
            obj.shadeMode     = static_cast<u32>(m_ShadeMode);
            // entityID is 1-indexed so the fragment shader output matches m_EntityLookup
            obj.entityID      = (u32)m_EntityLookup.size();  // assigned before push_back
            obj.boneOffset    = 0;

            m_EntityLookup.push_back(entity);      // m_EntityLookup[count + 1] = entity
            m_EntityToSSBOIndex[entity] = count;   // entity → 0-based SSBO index

            // Skinned mesh: get bone offset from Animation component on this entity
            if (meshesData[mr.MeshIndex].IsSkinned && registry.all_of<Animation>(entity))
            {
                auto& anim = registry.get<Animation>(entity);
                if (anim.BufferAllocated)
                    obj.boneOffset = anim.BoneBufferOffset;
            }

            auto* ib = std::static_pointer_cast<VKIndexBuffer>(mesh->GetIndexBuffer()).get();
            obj.indexCount   = ib ? ib->GetCount() : 0;
            obj.firstIndex   = 0;
            obj.vertexOffset = 0;
            obj._pad         = 0;

            // Indirect command — instanceCount=1; per-region GPU cull zeros it if culled.
            // firstInstance = SSBO index (gl_BaseInstance in shader → objects[gl_BaseInstance]).
            // Duplicate into all k_IndirectRegionCount regions (camera + 4 cascades) so each
            // region has its own independently-cullable command for this object.
            VkDrawIndexedIndirectCommand baseCmd{};
            baseCmd.indexCount    = obj.indexCount;
            baseCmd.instanceCount = 1;
            baseCmd.firstIndex    = 0;
            baseCmd.vertexOffset  = 0;
            baseCmd.firstInstance = count;
            for (u32 r = 0; r < k_IndirectRegionCount; ++r)
                indirectCmds[r * k_IndirectRegionStride + count] = baseCmd;
            count++;
        }

        m_GPUObjectCount = count;
    }

    void RenderingSystem::UpdateGlobalUniforms()
    {
        GlobalUniforms ubo{};
        ubo.view = m_CameraParams.view;
        ubo.projection = m_CameraParams.projection;
        ubo.projection[1][1] *= -1.0f;  // Vulkan Y-flip (shader only, not ImGuizmo)
        ubo.viewProjection = ubo.projection * ubo.view;
        ubo.cameraPos = m_CameraParams.position;
        ubo.time = Time::GetTime();
        for (u32 i = 0; i < k_ShadowCascadeCount; ++i)
            ubo.lightSpaceMatrix[i] = m_Cascades.lightSpaceMatrix[i];
        ubo.cascadeSplitsViewZ = m_Cascades.splitsViewZ;
        // Negative bias (sentinel) disables shadows entirely in the PBR shader.
        ubo.shadowBias       = m_ShadowParams.castShadows ? m_ShadowParams.shadowBias : glm::vec4(-1.0f);
        ubo.shadowNormalBias = m_ShadowParams.shadowNormalBias;
        ubo.cascadeTexelSize = m_Cascades.texelSize;
        ubo.iblIntensity    = m_CameraParams.iblIntensity;
        ubo.skyboxIntensity = m_CameraParams.skyboxIntensity;
        ubo.debugVisualizeCascades = m_ShadowParams.debugVisualizeCascades ? 1.0f : 0.0f;
        ubo.cascadeBlendWidth      = m_ShadowParams.cascadeBlendWidth;

        m_GlobalUniformBuffer->SetData(&ubo, sizeof(GlobalUniforms));
        m_CachedViewProj = ubo.viewProjection;
    }

    // =========================================================================
    // Main Update
    // =========================================================================

    void RenderingSystem::Update(Scene* scene)
    {
        LH_PROFILE_FUNCTION();
        auto& registry = scene->Registry();

        // Drain pending shader reloads (queued by FileWatcher on background thread)
        {
            std::lock_guard lock(m_ReloadMutex);
            for (const auto& name : m_PendingReloads)
            {
                LH_CORE_INFO("Shader file changed — reloading '{}'", name);
                ShaderLibrary::Reload(name);
            }
            m_PendingReloads.clear();

            if (m_PendingUtilityReload)
            {
                LH_CORE_INFO("Utility shader changed — recompiling all utility shaders");
                m_Pipeline->RecompileUtilityShaders();
                m_PendingUtilityReload = false;
            }
        }

        m_FrameAllocator->Reset();

        // --- Frame Debugger: Frozen state ---
        // Phase 14C — strict snapshot model with auto-recapture on camera move.
        //
        // While Frozen, the live render graph is NOT rebuilt or re-executed.
        // The LDR output target retains the LAST CAPTURED image (no other code
        // writes it in this state), so the editor's ScenePanel — which samples
        // it through ImGui — keeps showing the GPU-true captured frame.
        //
        // Each Frozen tick we cheaply recompute the camera viewProj (no GPU
        // upload) and bit-compare against captureViewProj. If different, the
        // user has moved the camera, so we flip the state machine back to
        // CaptureRequested and fall through to the normal capture flow below;
        // FrameDebugger::BeginCapture will tear down the prior archives.
        if (m_FrameDebugger.state == DebuggerState::Frozen)
        {
            if (Renderer::GetBackend()->GetAPI() != RenderBackend::API::Vulkan) return;

            // Mirror the Vulkan Y-flip applied in UpdateGlobalUniforms so the
            // comparison matches what the GPU actually saw at capture time.
            glm::mat4 currentProj = m_CameraParams.projection;
            currentProj[1][1] *= -1.0f;
            glm::mat4 currentViewProj = currentProj * m_CameraParams.view;

            const bool cameraMoved = std::memcmp(&currentViewProj,
                                                  &m_FrameDebugger.capturedFrame.captureViewProj,
                                                  sizeof(glm::mat4)) != 0;

            if (!cameraMoved)
            {
                // Static — minimal graph: just blit ImGui to the swapchain.
                // The editor panel reads the LDR output through ImGui::Image; the
                // image's persistent layout (SHADER_READ_ONLY_OPTIMAL after the
                // capture's outline pass) is preserved across frames.
                m_Pipeline->ExecuteMinimal();
                return;
            }

            // Camera moved — re-trigger capture and fall through. BeginCapture
            // (called below before ExecuteGraph) destroys the prior archives.
            m_FrameDebugger.state = DebuggerState::CaptureRequested;
        }

        // --- Frame Debugger: Prepare for capture (BeginCapture below handles reset) ---
        // Capture metadata + GPU archives are reset together inside FrameDebugger::
        // BeginCapture (called after rg.Compile, just before ExecuteGraph) so prior
        // archives get freed in the right order.

        if (Renderer::GetBackend()->GetAPI() == RenderBackend::API::Vulkan)
        {
            // Light uniforms first (sets m_Cascades.lightSpaceMatrix)
            UpdateLightUniforms(scene);
            // Global UBO second (reads m_Cascades.lightSpaceMatrix)
            UpdateGlobalUniforms();

            // Register materials and hold assets for all visible entities
            auto matView = registry.view<WorldTransform, MeshRenderer>();
            for (auto [entity, wt, mr] : matView.each())
            {
                if (mr.ModelUUID.IsValid())
                {
                    auto model = AssetManager::GetAsset<Model>(mr.ModelUUID);
                    if (model)
                        scene->HoldAsset(mr.ModelUUID, model);
                }
                if (!mr.MaterialUUID.IsValid()) continue;
                auto material = AssetManager::GetAsset<Material>(mr.MaterialUUID);
                if (material)
                {
                    scene->HoldAsset(mr.MaterialUUID, material);
                    EnsureMaterialRegistered(material);
                }
            }

            // Upload dirty materials
            MaterialSystem::Update(VK_NULL_HANDLE);

            // Upload post-process settings
            UpdatePostProcessUBO();
            UpdateGTAOUBO();

            // Build GPU object buffer (after materials are registered)
            BuildGPUObjectBuffer(registry);

            // Partition entities into opaque/cutout/transparent draw buckets.
            // Must follow BuildGPUObjectBuffer so gpuObjectIndex/entityIndex
            // reference the freshly populated indirect buffer.
            m_DrawListBuilder.Build(registry, m_MaterialSlotMap, m_EntityToSSBOIndex, m_DrawList);

            // Build + execute the render graph (graph assembly lives in RenderPipeline).
            m_Pipeline->Execute(registry);

            // --- Mouse picking readback (immediate, single pixel) ---
            if (m_PickPending)
            {
                m_PickPending = false;
                int px = m_PickCoord.x;
                int py = m_PickCoord.y;

                if (px >= 0 && py >= 0 && px < (int)m_Targets.GetEntityIDBuffer()->GetWidth() && py < (int)m_Targets.GetEntityIDBuffer()->GetHeight())
                {
                    auto vkID = std::static_pointer_cast<VKTexture>(m_Targets.GetEntityIDBuffer());

                    // Create a small staging buffer for readback
                    VkBuffer stagingBuf;
                    VkBufferCreateInfo bufInfo{ VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
                    bufInfo.size = sizeof(u32);
                    bufInfo.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
                    VmaAllocation stagingAlloc = VulkanAllocator::AllocateBuffer(bufInfo, VMA_MEMORY_USAGE_GPU_TO_CPU, stagingBuf);

                    VulkanContext::Get().ImmediateSubmit([&](VkCommandBuffer cmd)
                    {
                        // Transition entity ID image to transfer src
                        VkImageMemoryBarrier2 barrier{ VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2 };
                        barrier.srcStageMask  = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
                        barrier.srcAccessMask = VK_ACCESS_2_SHADER_READ_BIT;
                        barrier.dstStageMask  = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
                        barrier.dstAccessMask = VK_ACCESS_2_TRANSFER_READ_BIT;
                        barrier.oldLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
                        barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
                        barrier.image = vkID->GetImage();
                        barrier.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };

                        VkDependencyInfo dep{ VK_STRUCTURE_TYPE_DEPENDENCY_INFO };
                        dep.imageMemoryBarrierCount = 1;
                        dep.pImageMemoryBarriers = &barrier;
                        vkCmdPipelineBarrier2(cmd, &dep);

                        // Copy single pixel
                        VkBufferImageCopy region{};
                        region.bufferOffset = 0;
                        region.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
                        region.imageOffset = { px, py, 0 };
                        region.imageExtent = { 1, 1, 1 };
                        vkCmdCopyImageToBuffer(cmd, vkID->GetImage(), VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, stagingBuf, 1, &region);
                    });

                    // Read the result
                    void* mapped = VulkanAllocator::Map(stagingAlloc);
                    u32 entityIdx = *reinterpret_cast<u32*>(mapped);
                    VulkanAllocator::Unmap(stagingAlloc);
                    VulkanAllocator::FreeBuffer(stagingBuf, stagingAlloc);

                    if (entityIdx > 0 && entityIdx < (u32)m_EntityLookup.size())
                        m_PickedEntity = m_EntityLookup[entityIdx];
                    else
                        m_PickedEntity = entt::null;

                    m_PickResultReady = true;
                }
            }

            return;
        }
    }

    std::shared_ptr<Texture> RenderingSystem::GetNamedTexture(const std::string& name) const
    {
        auto it = m_NamedTextures.find(name);
        return (it != m_NamedTextures.end()) ? it->second : nullptr;
    }

    // =========================================================================
    // Mouse Picking
    // =========================================================================

    void RenderingSystem::RequestPick(int x, int y)
    {
        m_PickCoord = { x, y };
        m_PickPending = true;
        m_PickResultReady = false;
    }

    entt::entity RenderingSystem::ConsumePickResult()
    {
        m_PickResultReady = false;
        return m_PickedEntity;
    }

    // =========================================================================
    // Resize
    // =========================================================================

    void RenderingSystem::Resize(u32 width, u32 height)
    {
        // Guard against unsigned underflow from negative float→u32 casts at startup
        if (m_Targets.IsAllocated() && width > 0 && height > 0 && width <= 16384 && height <= 16384)
        {
            m_Targets.Resize(width, height);
            m_Pipeline->OnResize(width, height);
        }
    }

    // =========================================================================
    //  Frame Debugger: Re-Recording (Frozen State)
    // =========================================================================

    void RenderingSystem::InitDebugBlitResources()
    {
        if (m_FrameDebugger.blitPipeline) return; // Already initialized

        auto shadersPath = FileSystem::EngineAssetsPath("shaders");
        m_FrameDebugger.blitFragSpv  = ShaderCompiler::Compile(shadersPath / "debugBlit.frag");
        m_FrameDebugger.depthFragSpv = ShaderCompiler::Compile(shadersPath / "debugDepth.frag");

        if (m_FrameDebugger.blitFragSpv.empty() || m_FrameDebugger.depthFragSpv.empty() || m_FullscreenVertSpv.empty())
        {
            LH_CORE_ERROR("Failed to compile debug blit shaders");
            return;
        }

        auto device = VulkanContext::Get().GetDevice();

        // Create sampler
        VkSamplerCreateInfo samplerCI{};
        samplerCI.sType     = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
        samplerCI.magFilter = VK_FILTER_LINEAR;
        samplerCI.minFilter = VK_FILTER_LINEAR;
        samplerCI.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        samplerCI.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        samplerCI.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        vkCreateSampler(device, &samplerCI, nullptr, &m_FrameDebugger.sampler);

        // Descriptor set layout: binding 0 = combined image sampler
        VkDescriptorSetLayoutBinding binding{};
        binding.binding         = 0;
        binding.descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        binding.descriptorCount = 1;
        binding.stageFlags      = VK_SHADER_STAGE_FRAGMENT_BIT;

        VkDescriptorSetLayoutCreateInfo layoutCI{};
        layoutCI.sType        = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        layoutCI.bindingCount = 1;
        layoutCI.pBindings    = &binding;
        vkCreateDescriptorSetLayout(device, &layoutCI, nullptr, &m_FrameDebugger.descSetLayout);

        // Descriptor pool
        VkDescriptorPoolSize poolSize{ VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1 };
        VkDescriptorPoolCreateInfo poolCI{};
        poolCI.sType         = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
        poolCI.maxSets       = 1;
        poolCI.poolSizeCount = 1;
        poolCI.pPoolSizes    = &poolSize;
        poolCI.flags         = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
        vkCreateDescriptorPool(device, &poolCI, nullptr, &m_FrameDebugger.descPool);

        // Allocate descriptor set
        VkDescriptorSetAllocateInfo allocCI{};
        allocCI.sType              = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        allocCI.descriptorPool     = m_FrameDebugger.descPool;
        allocCI.descriptorSetCount = 1;
        allocCI.pSetLayouts        = &m_FrameDebugger.descSetLayout;
        vkAllocateDescriptorSets(device, &allocCI, &m_FrameDebugger.descSet);

        // Create blit pipeline (color)
        std::vector<VkDescriptorSetLayout> layouts = { m_FrameDebugger.descSetLayout };
        PipelineConfig blitConfig;
        blitConfig.depthTest  = false;
        blitConfig.depthWrite = false;
        blitConfig.cullMode         = VK_CULL_MODE_NONE;
        blitConfig.colorFormats     = { VK_FORMAT_R8G8B8A8_UNORM };
        m_FrameDebugger.blitPipeline = std::make_unique<VKPipeline>(
            blitConfig, m_FullscreenVertSpv, m_FrameDebugger.blitFragSpv, layouts);

        // Create depth visualization pipeline
        PipelineConfig depthConfig;
        depthConfig.depthTest  = false;
        depthConfig.depthWrite = false;
        depthConfig.cullMode         = VK_CULL_MODE_NONE;
        depthConfig.colorFormats     = { VK_FORMAT_R8G8B8A8_UNORM };

        VkPushConstantRange depthPC{};
        depthPC.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
        depthPC.offset     = 0;
        depthPC.size       = sizeof(float) * 2; // near, far
        depthConfig.pushConstantRanges = { depthPC };

        m_FrameDebugger.depthPipeline = std::make_unique<VKPipeline>(
            depthConfig, m_FullscreenVertSpv, m_FrameDebugger.depthFragSpv, layouts);
    }

    RG::ResourceHandle RenderingSystem::AddDebugBlitPass(RG::RenderGraph& rg, RG::ResourceHandle inputHandle, bool isDepth)
    {
        if (!m_FrameDebugger.blitPipeline || !m_Targets.GetLDROutput()) return inputHandle;

        struct DebugBlitData {
            RG::ResourceHandle output;
            RG::ResourceHandle input;
        };

        RG::ResourceHandle outputHandle;

        rg.AddPass<DebugBlitData>("DebugDisplayBlit",
            [&](DebugBlitData& data, RG::RenderPassBuilder& builder)
            {
                auto ldrVk = std::static_pointer_cast<VKTexture>(m_Targets.GetLDROutput());
                RG::TextureDesc desc;
                desc.name   = "LDROutput";
                desc.width  = m_Targets.GetLDROutput()->GetWidth();
                desc.height = m_Targets.GetLDROutput()->GetHeight();
                desc.format = RG::TextureFormat::RGBA8_Unorm;

                data.output = rg.ImportResource(desc,
                    (void*)ldrVk->GetImage(), (void*)ldrVk->GetImageView(),
                    RG::ResourceState::ShaderResource);
                data.output = builder.Write(data.output);

                data.input = builder.Read(inputHandle);
                outputHandle = data.output;
            },
            [this, isDepth](DebugBlitData& data, RG::RenderPassContext& ctx)
            {
                VkCommandBuffer cmd = ctx.commandBuffer;

                u32 w = m_Targets.GetLDROutput()->GetWidth();
                u32 h = m_Targets.GetLDROutput()->GetHeight();
                VkViewport vp{}; vp.width = (float)w; vp.height = (float)h; vp.maxDepth = 1.0f;
                vkCmdSetViewport(cmd, 0, 1, &vp);
                VkRect2D sc{}; sc.extent = { w, h };
                vkCmdSetScissor(cmd, 0, 1, &sc);

                if (isDepth && m_FrameDebugger.depthPipeline)
                {
                    m_FrameDebugger.depthPipeline->Bind(cmd);
                    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                        m_FrameDebugger.depthPipeline->GetLayout(), 0, 1, &m_FrameDebugger.descSet, 0, nullptr);

                    float pc[2] = { 0.1f, 200.0f }; // near/far for shadow maps
                    vkCmdPushConstants(cmd, m_FrameDebugger.depthPipeline->GetLayout(),
                        VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(pc), pc);
                }
                else
                {
                    m_FrameDebugger.blitPipeline->Bind(cmd);
                    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                        m_FrameDebugger.blitPipeline->GetLayout(), 0, 1, &m_FrameDebugger.descSet, 0, nullptr);
                }

                vkCmdDraw(cmd, 3, 1, 0, 0);
            }
        );

        return outputHandle;
    }

    // Phase 14C — RenderingSystem::RenderCapturedFrame removed.
    // Live re-replay was the source of the original sync bug. The Frozen-state
    // branch (top of Update) now serves archived images; per-draw inspection
    // is implemented below via ImmediateSubmit replay-then-copy.

    // =========================================================================
    //  Frame Debugger Phase 14E — Per-draw replay-then-copy
    // =========================================================================

    void RenderingSystem::EnsurePerDrawPreviewTexture(u32 width, u32 height)
    {
        if (m_PerDrawPreviewImage != VK_NULL_HANDLE
            && m_PerDrawPreviewWidth == width
            && m_PerDrawPreviewHeight == height) return;

        // Tear down any prior preview at a different size first. Deferred so
        // an in-flight ImGui frame can finish sampling the old view safely.
        if (m_PerDrawPreviewImage != VK_NULL_HANDLE)
        {
            VkImage       img   = m_PerDrawPreviewImage;
            VkImageView   view  = m_PerDrawPreviewView;
            VmaAllocation alloc = m_PerDrawPreviewAlloc;
            VulkanContext::Get().PushDeletion([img, view, alloc]() {
                auto dev = VulkanContext::Get().GetDevice();
                vkDestroyImageView(dev, view, nullptr);
                VulkanAllocator::FreeImage(img, alloc);
            });
            m_PerDrawPreviewImage = VK_NULL_HANDLE;
            m_PerDrawPreviewView  = VK_NULL_HANDLE;
            m_PerDrawPreviewAlloc = nullptr;
        }

        VkImageCreateInfo ci{ VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
        ci.imageType     = VK_IMAGE_TYPE_2D;
        ci.extent        = { width, height, 1 };
        ci.mipLevels     = 1;
        ci.arrayLayers   = 1;
        // Match the scene color target's format so vkCmdCopyImage is layout-compatible.
        ci.format        = VK_FORMAT_R16G16B16A16_SFLOAT;
        ci.tiling        = VK_IMAGE_TILING_OPTIMAL;
        ci.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        ci.usage         = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
        ci.samples       = VK_SAMPLE_COUNT_1_BIT;
        ci.sharingMode   = VK_SHARING_MODE_EXCLUSIVE;

        m_PerDrawPreviewAlloc = VulkanAllocator::AllocateImage(ci, VMA_MEMORY_USAGE_AUTO, m_PerDrawPreviewImage);

        VkImageViewCreateInfo vci{ VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
        vci.image            = m_PerDrawPreviewImage;
        vci.viewType         = VK_IMAGE_VIEW_TYPE_2D;
        vci.format           = ci.format;
        vci.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
        vkCreateImageView(VulkanContext::Get().GetDevice(), &vci, nullptr, &m_PerDrawPreviewView);

        m_PerDrawPreviewWidth  = width;
        m_PerDrawPreviewHeight = height;
        m_PerDrawPreviewKey    = UINT64_MAX;  // dimensions changed → invalidate cache
    }

    void RenderingSystem::DestroyPerDrawPreviewTexture()
    {
        if (m_PerDrawPreviewImage == VK_NULL_HANDLE) return;
        auto dev = VulkanContext::Get().GetDevice();
        vkDestroyImageView(dev, m_PerDrawPreviewView, nullptr);
        VulkanAllocator::FreeImage(m_PerDrawPreviewImage, m_PerDrawPreviewAlloc);
        m_PerDrawPreviewImage  = VK_NULL_HANDLE;
        m_PerDrawPreviewView   = VK_NULL_HANDLE;
        m_PerDrawPreviewAlloc  = nullptr;
        m_PerDrawPreviewWidth  = 0;
        m_PerDrawPreviewHeight = 0;
        m_PerDrawPreviewKey    = UINT64_MAX;
    }

    void RenderingSystem::ReplayPassUpToDraw(u32 passIdx, u32 localDrawIdx)
    {
        if (m_FrameDebugger.state != DebuggerState::Frozen) return;
        if (!m_FrameDebugger.capturedFrame.valid) return;
        if (passIdx >= m_FrameDebugger.capturedFrame.passes.size()) return;

        const auto& pass = m_FrameDebugger.capturedFrame.passes[passIdx];

        // v1 — only GeometryPass per-draw replay is wired. Other passes leave
        // the cache key unchanged so the panel falls back to pass-output archive.
        if (pass.name != "GeometryPass") return;
        if (!m_Targets.GetSceneColor() || !m_Targets.GetSceneDepth() || !m_Targets.GetEntityIDBuffer()) return;

        // Cache hit — same selection as last replay, nothing to do.
        const u64 key = ((u64)passIdx << 32) | (u64)localDrawIdx;
        if (key == m_PerDrawPreviewKey) return;

        const u32 width  = m_Targets.GetSceneColor()->GetWidth();
        const u32 height = m_Targets.GetSceneColor()->GetHeight();
        EnsurePerDrawPreviewTexture(width, height);
        if (m_PerDrawPreviewImage == VK_NULL_HANDLE) return;

        auto vkSceneColor = std::static_pointer_cast<VKTexture>(m_Targets.GetSceneColor());
        auto vkSceneDepth = std::static_pointer_cast<VKTexture>(m_Targets.GetSceneDepth());
        auto vkEntityID   = std::static_pointer_cast<VKTexture>(m_Targets.GetEntityIDBuffer());
        VkImage     sceneColorImg  = vkSceneColor->GetImage();
        VkImageView sceneColorView = vkSceneColor->GetImageView();
        VkImage     sceneDepthImg  = vkSceneDepth->GetImage();
        VkImageView sceneDepthView = vkSceneDepth->GetImageView();
        VkImage     entityIDImg    = vkEntityID->GetImage();
        VkImageView entityIDView   = vkEntityID->GetImageView();

        // Total draws to issue (1-based count). Original GeometryPass emits
        // draws in opaque → cutout → transparent order, which matches the
        // capture-time CapturedDrawCall ordering.
        const u32 maxDraws = localDrawIdx + 1;

        // Capture the CPU-side data we need by value (the lambda runs inside
        // ImmediateSubmit and must be self-contained).
        VkPolygonMode polyMode = (m_ShadeMode == ShadeMode::Wireframe) ? VK_POLYGON_MODE_LINE : VK_POLYGON_MODE_FILL;
        UUID pbrUUID = ShaderLibrary::Get("pbr")->Handle;

        VulkanContext::Get().ImmediateSubmit([&, this](VkCommandBuffer cmd)
        {
            // ---- Phase 1: prep all attachments + preview ----
            // We use UNDEFINED → ATTACHMENT to ignore the prior contents (the
            // graph CLEARs anyway) and avoid relying on whatever layout the
            // last live frame left them in.
            VkImageMemoryBarrier2 prep[4]{};
            auto fillAtt = [](VkImageMemoryBarrier2& b, VkImage img,
                              VkImageAspectFlags aspect, VkImageLayout newLayout,
                              VkPipelineStageFlags2 dstStage, VkAccessFlags2 dstAccess)
            {
                b.sType               = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
                b.srcStageMask        = VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT;
                b.srcAccessMask       = 0;
                b.dstStageMask        = dstStage;
                b.dstAccessMask       = dstAccess;
                b.oldLayout           = VK_IMAGE_LAYOUT_UNDEFINED;
                b.newLayout           = newLayout;
                b.image               = img;
                b.subresourceRange    = { aspect, 0, 1, 0, 1 };
                b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            };
            fillAtt(prep[0], sceneColorImg, VK_IMAGE_ASPECT_COLOR_BIT,
                    VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                    VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                    VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT);
            fillAtt(prep[1], sceneDepthImg, VK_IMAGE_ASPECT_DEPTH_BIT,
                    VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
                    VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT,
                    VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT);
            fillAtt(prep[2], entityIDImg, VK_IMAGE_ASPECT_COLOR_BIT,
                    VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                    VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                    VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT);

            VkDependencyInfo depPrep{ VK_STRUCTURE_TYPE_DEPENDENCY_INFO };
            depPrep.imageMemoryBarrierCount = 3;
            depPrep.pImageMemoryBarriers    = prep;
            vkCmdPipelineBarrier2(cmd, &depPrep);

            // ---- Phase 2: BeginRendering with cleared attachments ----
            AttachmentInfo colorAtt[2]{};
            colorAtt[0].ImageView  = sceneColorView;
            colorAtt[0].Format     = VK_FORMAT_R16G16B16A16_SFLOAT;
            colorAtt[0].LoadOp     = VK_ATTACHMENT_LOAD_OP_CLEAR;
            colorAtt[0].StoreOp    = VK_ATTACHMENT_STORE_OP_STORE;
            colorAtt[0].ClearValue = { { {0.f, 0.f, 0.f, 0.f} } };
            colorAtt[0].Layout     = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

            colorAtt[1].ImageView  = entityIDView;
            colorAtt[1].Format     = VK_FORMAT_R32_UINT;
            colorAtt[1].LoadOp     = VK_ATTACHMENT_LOAD_OP_CLEAR;
            colorAtt[1].StoreOp    = VK_ATTACHMENT_STORE_OP_STORE;
            colorAtt[1].ClearValue.color.uint32[0] = 0;
            colorAtt[1].Layout     = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

            AttachmentInfo depthAtt{};
            depthAtt.ImageView  = sceneDepthView;
            depthAtt.Format     = VK_FORMAT_D32_SFLOAT;
            depthAtt.LoadOp     = VK_ATTACHMENT_LOAD_OP_CLEAR;
            depthAtt.StoreOp    = VK_ATTACHMENT_STORE_OP_STORE;
            depthAtt.ClearValue.depthStencil = { 1.0f, 0 };
            depthAtt.Layout     = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;

            RenderPassInfo rpInfo{};
            rpInfo.ColorAttachments = std::span<AttachmentInfo>(colorAtt, 2);
            rpInfo.DepthAttachment  = &depthAtt;
            rpInfo.RenderArea       = { {0, 0}, { width, height } };

            DynamicRendering::BeginRendering(cmd, rpInfo);

            // ---- Phase 3: bind pipelines + descriptors, replay draws ----
            // Same descriptor sets as the live GeometryPass — the underlying
            // UBOs/SSBOs are byte-stable in Frozen state (no live writers).
            auto* opaquePipeline = m_GeoPipelineManager.GetOrCreate(
                pbrUUID, Material::RenderMode::Opaque, Material::CullMode::Back,
                polyMode, m_PBRVertSpv, m_PBRFragSpv);
            if (!opaquePipeline)
            {
                DynamicRendering::EndRendering(cmd);
                return;
            }
            VkPipelineLayout pipelineLayout = opaquePipeline->GetLayout();

            VkDescriptorSet bindlessSet = VulkanContext::Get().GetBindlessSet().GetSet();
            VkDescriptorSet sets[] = {
                m_GlobalDescriptorSet, bindlessSet, MaterialSystem::GetDescriptorSet(),
                m_LightDescSet, BoneMatrixBuffer::GetDescriptorSet(), m_ObjectSSBODescSet
            };
            vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                pipelineLayout, 0, 6, sets, 0, nullptr);

            VkViewport vp{}; vp.width = (float)width; vp.height = (float)height; vp.maxDepth = 1.0f;
            vkCmdSetViewport(cmd, 0, 1, &vp);
            VkRect2D scRect{}; scRect.extent = { width, height };
            vkCmdSetScissor(cmd, 0, 1, &scRect);

            u32 drawsRemaining = maxDraws;

            auto ReplayBatch = [&](const std::vector<DrawCommand>& draws, Material::RenderMode mode)
            {
                if (drawsRemaining == 0 || draws.empty()) return;

                Material::CullMode currentCull = Material::CullMode::Back;
                bool currentSkinned = false;
                auto* pipeline = m_GeoPipelineManager.GetOrCreate(
                    pbrUUID, mode, currentCull, polyMode, m_PBRVertSpv, m_PBRFragSpv);
                if (!pipeline) return;
                pipeline->Bind(cmd);

                for (const auto& dc : draws)
                {
                    if (drawsRemaining == 0) return;

                    if (dc.cullMode != currentCull || dc.isSkinned != currentSkinned)
                    {
                        currentCull    = dc.cullMode;
                        currentSkinned = dc.isSkinned;
                        VKPipeline* newPipeline = currentSkinned
                            ? m_GeoSkinnedPipelineManager.GetOrCreate(pbrUUID, mode, currentCull, polyMode, m_PBRSkinnedVertSpv, m_PBRFragSpv)
                            : m_GeoPipelineManager.GetOrCreate       (pbrUUID, mode, currentCull, polyMode, m_PBRVertSpv,        m_PBRFragSpv);
                        if (!newPipeline) continue;
                        newPipeline->Bind(cmd);
                    }

                    auto mesh = dc.model->GetMesh(dc.meshIndex);
                    if (!mesh) { drawsRemaining--; continue; }
                    auto vb = std::static_pointer_cast<VKVertexBuffer>(mesh->GetVertexBuffer());
                    auto ib = std::static_pointer_cast<VKIndexBuffer >(mesh->GetIndexBuffer ());
                    if (!vb || !ib) { drawsRemaining--; continue; }

                    VkBuffer vbuf[] = { vb->GetVulkanBuffer() };
                    VkDeviceSize offsets[] = { 0 };
                    vkCmdBindVertexBuffers(cmd, 0, 1, vbuf, offsets);
                    vkCmdBindIndexBuffer(cmd, ib->GetVulkanBuffer(), 0, VK_INDEX_TYPE_UINT32);

                    // Use the captured indirect buffer (camera region, byte-stable
                    // in Frozen state). gpuObjectIndex is also the firstInstance
                    // base — same as live GeometryPass.
                    VkDeviceSize indirectOffset = dc.gpuObjectIndex * sizeof(VkDrawIndexedIndirectCommand);
                    vkCmdDrawIndexedIndirect(cmd, m_IndirectBuffer, indirectOffset, 1,
                        sizeof(VkDrawIndexedIndirectCommand));

                    --drawsRemaining;
                }
            };

            ReplayBatch(m_DrawList.opaque,      Material::RenderMode::Opaque);
            ReplayBatch(m_DrawList.cutout,      Material::RenderMode::Cutout);
            ReplayBatch(m_DrawList.transparent, Material::RenderMode::Transparent);

            DynamicRendering::EndRendering(cmd);

            // ---- Phase 4: copy SceneColor → preview ----
            VkImageMemoryBarrier2 toCopy[2]{};
            toCopy[0].sType               = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
            toCopy[0].srcStageMask        = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
            toCopy[0].srcAccessMask       = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;
            toCopy[0].dstStageMask        = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
            toCopy[0].dstAccessMask       = VK_ACCESS_2_TRANSFER_READ_BIT;
            toCopy[0].oldLayout           = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
            toCopy[0].newLayout           = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
            toCopy[0].image               = sceneColorImg;
            toCopy[0].subresourceRange    = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
            toCopy[0].srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            toCopy[0].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;

            toCopy[1].sType               = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
            toCopy[1].srcStageMask        = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
            toCopy[1].srcAccessMask       = VK_ACCESS_2_SHADER_READ_BIT;
            toCopy[1].dstStageMask        = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
            toCopy[1].dstAccessMask       = VK_ACCESS_2_TRANSFER_WRITE_BIT;
            // First-ever replay is UNDEFINED, subsequent replays come from
            // SHADER_READ_ONLY_OPTIMAL after the panel sampled the previous
            // preview. Treat both uniformly via UNDEFINED — discards content
            // (which we'd overwrite anyway).
            toCopy[1].oldLayout           = VK_IMAGE_LAYOUT_UNDEFINED;
            toCopy[1].newLayout           = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
            toCopy[1].image               = m_PerDrawPreviewImage;
            toCopy[1].subresourceRange    = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
            toCopy[1].srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            toCopy[1].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;

            VkDependencyInfo depCopy{ VK_STRUCTURE_TYPE_DEPENDENCY_INFO };
            depCopy.imageMemoryBarrierCount = 2;
            depCopy.pImageMemoryBarriers    = toCopy;
            vkCmdPipelineBarrier2(cmd, &depCopy);

            VkImageCopy copy{};
            copy.srcSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
            copy.dstSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
            copy.extent         = { width, height, 1 };
            vkCmdCopyImage(cmd,
                sceneColorImg,         VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                m_PerDrawPreviewImage, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                1, &copy);

            // ---- Phase 5: restore layouts for next consumer ----
            // SceneColor → SHADER_READ_ONLY (matches what the live pipeline
            // expects after PostProcessPass; next live frame's barriers will
            // re-transition as needed).
            // Preview → SHADER_READ_ONLY for ImGui sampling.
            VkImageMemoryBarrier2 fin[2]{};
            fin[0].sType               = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
            fin[0].srcStageMask        = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
            fin[0].srcAccessMask       = VK_ACCESS_2_TRANSFER_READ_BIT;
            fin[0].dstStageMask        = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
            fin[0].dstAccessMask       = VK_ACCESS_2_SHADER_READ_BIT;
            fin[0].oldLayout           = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
            fin[0].newLayout           = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            fin[0].image               = sceneColorImg;
            fin[0].subresourceRange    = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
            fin[0].srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            fin[0].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;

            fin[1].sType               = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
            fin[1].srcStageMask        = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
            fin[1].srcAccessMask       = VK_ACCESS_2_TRANSFER_WRITE_BIT;
            fin[1].dstStageMask        = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
            fin[1].dstAccessMask       = VK_ACCESS_2_SHADER_READ_BIT;
            fin[1].oldLayout           = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
            fin[1].newLayout           = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            fin[1].image               = m_PerDrawPreviewImage;
            fin[1].subresourceRange    = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
            fin[1].srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            fin[1].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;

            VkDependencyInfo depFin{ VK_STRUCTURE_TYPE_DEPENDENCY_INFO };
            depFin.imageMemoryBarrierCount = 2;
            depFin.pImageMemoryBarriers    = fin;
            vkCmdPipelineBarrier2(cmd, &depFin);
        });

        m_PerDrawPreviewKey = key;
    }

    // =========================================================================
    //  Frame Debugger Phase 14F — Depth archive visualization (CSM cascades)
    // =========================================================================

    void RenderingSystem::EnsureDepthPreviewTexture(u32 width, u32 height)
    {
        if (m_DepthPreviewImage != VK_NULL_HANDLE
            && m_DepthPreviewWidth == width
            && m_DepthPreviewHeight == height) return;

        if (m_DepthPreviewImage != VK_NULL_HANDLE)
        {
            VkImage       img   = m_DepthPreviewImage;
            VkImageView   view  = m_DepthPreviewView;
            VmaAllocation alloc = m_DepthPreviewAlloc;
            VulkanContext::Get().PushDeletion([img, view, alloc]() {
                auto dev = VulkanContext::Get().GetDevice();
                vkDestroyImageView(dev, view, nullptr);
                VulkanAllocator::FreeImage(img, alloc);
            });
            m_DepthPreviewImage = VK_NULL_HANDLE;
            m_DepthPreviewView  = VK_NULL_HANDLE;
            m_DepthPreviewAlloc = nullptr;
        }

        VkImageCreateInfo ci{ VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
        ci.imageType     = VK_IMAGE_TYPE_2D;
        ci.extent        = { width, height, 1 };
        ci.mipLevels     = 1;
        ci.arrayLayers   = 1;
        // RGBA8 — depth blit shader writes a tonemapped grayscale that ImGui
        // can sample directly without HDR clipping concerns.
        ci.format        = VK_FORMAT_R8G8B8A8_UNORM;
        ci.tiling        = VK_IMAGE_TILING_OPTIMAL;
        ci.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        ci.usage         = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT
                         | VK_IMAGE_USAGE_SAMPLED_BIT;
        ci.samples       = VK_SAMPLE_COUNT_1_BIT;
        ci.sharingMode   = VK_SHARING_MODE_EXCLUSIVE;

        m_DepthPreviewAlloc = VulkanAllocator::AllocateImage(ci, VMA_MEMORY_USAGE_AUTO, m_DepthPreviewImage);

        VkImageViewCreateInfo vci{ VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
        vci.image            = m_DepthPreviewImage;
        vci.viewType         = VK_IMAGE_VIEW_TYPE_2D;
        vci.format           = ci.format;
        vci.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
        vkCreateImageView(VulkanContext::Get().GetDevice(), &vci, nullptr, &m_DepthPreviewView);

        m_DepthPreviewWidth  = width;
        m_DepthPreviewHeight = height;
        m_DepthPreviewKey    = UINT64_MAX;
    }

    void RenderingSystem::DestroyDepthPreviewTexture()
    {
        if (m_DepthPreviewImage == VK_NULL_HANDLE) return;
        auto dev = VulkanContext::Get().GetDevice();
        vkDestroyImageView(dev, m_DepthPreviewView, nullptr);
        VulkanAllocator::FreeImage(m_DepthPreviewImage, m_DepthPreviewAlloc);
        m_DepthPreviewImage  = VK_NULL_HANDLE;
        m_DepthPreviewView   = VK_NULL_HANDLE;
        m_DepthPreviewAlloc  = nullptr;
        m_DepthPreviewWidth  = 0;
        m_DepthPreviewHeight = 0;
        m_DepthPreviewKey    = UINT64_MAX;
    }

    void RenderingSystem::BlitArchivedDepthToPreview(u32 archiveIdx, int layer, float nearZ, float farZ)
    {
        if (m_FrameDebugger.state != DebuggerState::Frozen) return;
        if (!m_FrameDebugger.capturedFrame.valid) return;
        if (archiveIdx >= m_FrameDebugger.capturedFrame.archivedImages.size()) return;

        auto& archive = m_FrameDebugger.capturedFrame.archivedImages[archiveIdx];
        if (!archive.isDepth || archive.image == VK_NULL_HANDLE) return;

        InitDebugBlitResources();  // idempotent — needed for sampler + depthPipeline + descSet
        if (!m_FrameDebugger.depthPipeline || m_FrameDebugger.descSet == VK_NULL_HANDLE) return;

        // Cache short-circuit (use layer+1 in the low bits so layer == -1 maps to 0).
        const u64 key = ((u64)archiveIdx << 32) | (u64)((layer < 0 ? 0u : (u32)layer + 1u));
        if (key == m_DepthPreviewKey && m_DepthPreviewImage != VK_NULL_HANDLE) return;

        // Preview matches archive dimensions so sampling = 1:1 texel mapping.
        EnsureDepthPreviewTexture(archive.width, archive.height);
        if (m_DepthPreviewImage == VK_NULL_HANDLE) return;

        // Resolve the source view.
        //
        // - Multi-layer archive + layer in range → per-layer view (sampled as 2D).
        // - Single-layer archive (e.g. cascade "ShadowMap.C<i>" — each cascade
        //   pass owns its own single-layer archive backed by the layer-i view of
        //   the shared 4-layer image) → archive.view, which is already 2D.
        // - layer < 0 → whole image (only meaningful for non-array archives).
        //
        // Falling back to archive.view when (layer >= archive.layers) is what
        // makes cascade slices renderable: the EventNode's archiveLayer holds
        // the *cascade index* (0..3) for detail-panel lookups, even though
        // the underlying archive only has 1 layer.
        VkDevice device = VulkanContext::Get().GetDevice();
        VkImageView srcView = (layer < 0 || archive.layers <= 1 || (u32)layer >= archive.layers)
            ? archive.view
            : archive.GetOrCreateLayerView(device, (u32)layer);
        if (srcView == VK_NULL_HANDLE) return;

        // Update the shared debug descriptor to point at this view. Safe to do
        // synchronously: ImmediateSubmit below blocks on a fence so the
        // descriptor isn't in flight while we're rewriting it.
        VkDescriptorImageInfo imgInfo{};
        imgInfo.sampler     = m_FrameDebugger.sampler;
        imgInfo.imageView   = srcView;
        imgInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

        VkWriteDescriptorSet write{ VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
        write.dstSet          = m_FrameDebugger.descSet;
        write.dstBinding      = 0;
        write.descriptorCount = 1;
        write.descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        write.pImageInfo      = &imgInfo;
        vkUpdateDescriptorSets(VulkanContext::Get().GetDevice(), 1, &write, 0, nullptr);

        const u32 width  = archive.width;
        const u32 height = archive.height;
        VkImage     dstImg  = m_DepthPreviewImage;
        VkImageView dstView = m_DepthPreviewView;

        VulkanContext::Get().ImmediateSubmit([this, dstImg, dstView, width, height, nearZ, farZ](VkCommandBuffer cmd)
        {
            // Preview UNDEFINED → COLOR_ATTACHMENT_OPTIMAL (clear-on-load).
            VkImageMemoryBarrier2 prep{ VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2 };
            prep.srcStageMask        = VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT;
            prep.srcAccessMask       = 0;
            prep.dstStageMask        = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
            prep.dstAccessMask       = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;
            prep.oldLayout           = VK_IMAGE_LAYOUT_UNDEFINED;
            prep.newLayout           = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
            prep.image               = dstImg;
            prep.subresourceRange    = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
            prep.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            prep.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;

            VkDependencyInfo depPrep{ VK_STRUCTURE_TYPE_DEPENDENCY_INFO };
            depPrep.imageMemoryBarrierCount = 1;
            depPrep.pImageMemoryBarriers    = &prep;
            vkCmdPipelineBarrier2(cmd, &depPrep);

            AttachmentInfo colorAtt{};
            colorAtt.ImageView  = dstView;
            colorAtt.Format     = VK_FORMAT_R8G8B8A8_UNORM;
            colorAtt.LoadOp     = VK_ATTACHMENT_LOAD_OP_CLEAR;
            colorAtt.StoreOp    = VK_ATTACHMENT_STORE_OP_STORE;
            colorAtt.ClearValue = { { {0.f, 0.f, 0.f, 1.f} } };
            colorAtt.Layout     = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

            RenderPassInfo rpInfo{};
            rpInfo.ColorAttachments = std::span<AttachmentInfo>(&colorAtt, 1);
            rpInfo.RenderArea       = { {0, 0}, { width, height } };

            DynamicRendering::BeginRendering(cmd, rpInfo);

            VkViewport vp{}; vp.width = (float)width; vp.height = (float)height; vp.maxDepth = 1.0f;
            vkCmdSetViewport(cmd, 0, 1, &vp);
            VkRect2D scRect{}; scRect.extent = { width, height };
            vkCmdSetScissor(cmd, 0, 1, &scRect);

            m_FrameDebugger.depthPipeline->Bind(cmd);
            vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                m_FrameDebugger.depthPipeline->GetLayout(), 0, 1, &m_FrameDebugger.descSet, 0, nullptr);

            float pc[2] = { nearZ, farZ };
            vkCmdPushConstants(cmd, m_FrameDebugger.depthPipeline->GetLayout(),
                VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(pc), pc);

            // Fullscreen triangle baked into the vertex shader (3 verts, no VB).
            vkCmdDraw(cmd, 3, 1, 0, 0);

            DynamicRendering::EndRendering(cmd);

            // Preview → SHADER_READ_ONLY for ImGui sampling.
            VkImageMemoryBarrier2 fin{ VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2 };
            fin.srcStageMask        = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
            fin.srcAccessMask       = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;
            fin.dstStageMask        = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
            fin.dstAccessMask       = VK_ACCESS_2_SHADER_READ_BIT;
            fin.oldLayout           = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
            fin.newLayout           = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            fin.image               = dstImg;
            fin.subresourceRange    = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
            fin.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            fin.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;

            VkDependencyInfo depFin{ VK_STRUCTURE_TYPE_DEPENDENCY_INFO };
            depFin.imageMemoryBarrierCount = 1;
            depFin.pImageMemoryBarriers    = &fin;
            vkCmdPipelineBarrier2(cmd, &depFin);
        });

        m_DepthPreviewKey = key;
    }
}
