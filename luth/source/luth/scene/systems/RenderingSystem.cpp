#include "luthpch.h"
#include "luth/scene/systems/RenderingSystem.h"
#include "luth/renderer/RenderPipeline.h"
#include "luth/jobs/JobSystem.h"
#include "luth/core/Profiler.h"
#include "luth/scene/Scene.h"
#include "luth/scene/Components.h"
#include "luth/renderer/Renderer.h"
#include "luth/renderer/material/MaterialSystem.h"
#include "luth/animation/BoneMatrixBuffer.h"
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

    std::shared_ptr<Texture> RenderingSystem::GetNamedTexture(const std::string& name) const
    {
        return m_Pipeline->GetNamedTexture(name);
    }

    void RenderingSystem::ReplayPassUpToDraw(u32 passIdx, u32 localDrawIdx)
    {
        m_Pipeline->ReplayPassUpToDraw(passIdx, localDrawIdx);
    }

    void RenderingSystem::BlitArchivedDepthToPreview(u32 archiveIdx, int layer, float nearZ, float farZ)
    {
        m_Pipeline->BlitArchivedDepthToPreview(archiveIdx, layer, nearZ, farZ);
    }

    const RG::RenderGraphSnapshot& RenderingSystem::GetGraphSnapshot() const
    {
        return m_Pipeline->GetGraphSnapshot();
    }

    void RenderingSystem::ExitCapture()
    {
        // Free GPU-owned archives BEFORE clearing the metadata vectors.
        m_FrameDebugger.DestroyArchives();
        m_FrameDebugger.state = DebuggerState::Inactive;
        m_FrameDebugger.capturedFrame.Clear();
        // Phase 14E — drop the per-draw replay cache key so the next capture
        // starts clean (preview texture itself is reused across captures).
        m_Pipeline->ResetPreviewCacheKeys();
    }

    VkImageView RenderingSystem::GetPerDrawPreviewView()   const { return m_Pipeline->GetPerDrawPreviewView(); }
    u64         RenderingSystem::GetPerDrawPreviewKey()    const { return m_Pipeline->GetPerDrawPreviewKey(); }
    u32         RenderingSystem::GetPerDrawPreviewWidth()  const { return m_Pipeline->GetPerDrawPreviewWidth(); }
    u32         RenderingSystem::GetPerDrawPreviewHeight() const { return m_Pipeline->GetPerDrawPreviewHeight(); }
    VkImageView RenderingSystem::GetDepthPreviewView()     const { return m_Pipeline->GetDepthPreviewView(); }
    u32         RenderingSystem::GetDepthPreviewWidth()    const { return m_Pipeline->GetDepthPreviewWidth(); }
    u32         RenderingSystem::GetDepthPreviewHeight()   const { return m_Pipeline->GetDepthPreviewHeight(); }

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

    void RenderingSystem::UpdateLightUniforms(Scene* scene)
    {
        LightUniforms lights{};
        m_LightGatherer.Gather(scene->Registry(), lights, m_ShadowParams);
        m_Pipeline->UploadLightUBO(lights);
        m_CascadeBuilder.Build(lights.dirLight.direction, m_CameraParams, m_ShadowParams, m_Cascades);
    }

    // =========================================================================
    // GPU Object Buffer + Cull Pipeline
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
            m_Pipeline->UpdateGlobalUniforms();

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
                    m_Pipeline->EnsureMaterialRegistered(material);
                }
            }

            // Upload dirty materials
            MaterialSystem::Update(VK_NULL_HANDLE);

            // Upload post-process settings
            m_Pipeline->UpdatePostProcessUBO();
            m_Pipeline->UpdateGTAOUBO();

            // Build GPU object buffer (after materials are registered)
            m_Pipeline->BuildGPUObjectBuffer(registry);

            // Partition entities into opaque/cutout/transparent draw buckets.
            // Must follow BuildGPUObjectBuffer so gpuObjectIndex/entityIndex
            // reference the freshly populated indirect buffer.
            m_DrawListBuilder.Build(registry, m_Pipeline->GetMaterialSlotMap(), m_Pipeline->GetEntityToSSBOIndex(), m_DrawList);

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

                    const auto& entityLookup = m_Pipeline->GetEntityLookup();
                    if (entityIdx > 0 && entityIdx < (u32)entityLookup.size())
                        m_PickedEntity = entityLookup[entityIdx];
                    else
                        m_PickedEntity = entt::null;

                    m_PickResultReady = true;
                }
            }

            return;
        }
    }

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

}
