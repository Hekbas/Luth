#include "luthpch.h"
#include "luth/renderer/subsystems/SkinningSubsystem.h"
#include "luth/renderer/RenderPipeline.h"
#include "luth/renderer/Renderer.h"
#include "luth/renderer/backend/vulkan/VulkanContext.h"
#include "luth/renderer/backend/vulkan/VulkanAccelerationStructure.h"
#include "luth/renderer/resources/BoneMatrixBuffer.h"
#include "luth/renderer/resources/Mesh.h"
#include "luth/renderer/resources/Model.h"
#include "luth/renderer/shader/ShaderLibrary.h"
#include "luth/renderer/rendergraph/RenderGraph.h"
#include "luth/scene/systems/RenderingSystem.h"
#include "luth/scene/systems/SystemRegistry.h"
#include "luth/resources/AssetManager.h"
#include "luth/core/diagnostics/Log.h"
#include "luth/core/RenderSnapshot.h"
#include "luth/core/FrameData.h"

namespace Luth
{
    namespace
    {
        // Push constant block — exact layout the GLSL `pc` struct expects (std430, 24 bytes total).
        struct SkinPC
        {
            VkDeviceAddress inputBda;     //  0
            VkDeviceAddress deformedBda;  //  8
            u32             vertexCount;  // 16
            u32             boneOffset;   // 20
        };
        static_assert(sizeof(SkinPC) == 24, "SkinPC must match skinning.comp push_constant layout");
        constexpr u32 LOCAL_SIZE_X = 64;
    }

    void SkinningSubsystem::Init(RenderPipeline& pipeline)
    {
        m_Pipeline = &pipeline;

        if (auto sh = ShaderLibrary::LoadEngine("shaders/skinning.comp"))
            m_Spv = sh->GetSpirV();
        if (m_Spv.empty())
        {
            LH_CORE_ERROR("SkinningSubsystem: failed to load shaders/skinning.comp");
            return;
        }

        VkPushConstantRange pcRange{ VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(SkinPC) };
        // Set 0 = BoneMatrixBuffer SSBO layout (shared with the main pipeline's Set 4).
        m_ComputePipeline = std::make_unique<VKComputePipeline>(
            m_Spv,
            std::vector<VkDescriptorSetLayout>{ BoneMatrixBuffer::GetDescriptorSetLayout() },
            std::vector<VkPushConstantRange>{ pcRange });
    }

    void SkinningSubsystem::Shutdown()
    {
        m_ComputePipeline.reset();
        m_Spv.clear();
        m_Pipeline = nullptr;
    }

    bool SkinningSubsystem::OnShaderReloaded(const std::string& name, const std::vector<u32>& spv)
    {
        if (name != "skinning.comp") return false;
        m_Spv = spv;

        VkPushConstantRange pcRange{ VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(SkinPC) };
        m_ComputePipeline = std::make_unique<VKComputePipeline>(
            m_Spv,
            std::vector<VkDescriptorSetLayout>{ BoneMatrixBuffer::GetDescriptorSetLayout() },
            std::vector<VkPushConstantRange>{ pcRange });
        LH_CORE_INFO("SkinningSubsystem: pipeline rebuilt after shader reload");
        return true;
    }

    void SkinningSubsystem::Dispatch(VkCommandBuffer cmd, const Mesh& mesh, u32 boneOffset) const
    {
        const auto& blas = mesh.GetBlas();
        if (!blas || !blas->IsSkinned()) return;
        if (blas->GetSkinInputBda() == 0 || blas->GetDeformedBda() == 0) return;

        SkinPC pc{};
        pc.inputBda    = blas->GetSkinInputBda();
        pc.deformedBda = blas->GetDeformedBda();
        pc.vertexCount = blas->GetVertexCount();
        pc.boneOffset  = boneOffset;

        vkCmdPushConstants(cmd, m_ComputePipeline->GetLayout(),
                           VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(SkinPC), &pc);

        const u32 groups = (pc.vertexCount + LOCAL_SIZE_X - 1) / LOCAL_SIZE_X;
        vkCmdDispatch(cmd, groups, 1, 1);
    }

    void SkinningSubsystem::DispatchAllSkinned(VkCommandBuffer cmd, const RenderSnapshot& snapshot) const
    {
        if (!m_ComputePipeline) return;

        bool boundPipeline = false;
        for (const auto& inst : snapshot.meshes)
        {
            if (!inst.isSkinned) continue;
            auto model = Luth::AssetManager::GetAsset<Model>(inst.modelUUID);
            if (!model) continue;
            auto mesh = model->GetMesh(inst.meshIndex);
            if (!mesh) continue;
            const auto& blas = mesh->GetBlas();
            if (!blas || !blas->IsSkinned()) continue;

            // Lazy bind so a snapshot with zero skinned meshes records zero compute commands.
            if (!boundPipeline)
            {
                m_ComputePipeline->Bind(cmd);
                const u32 slot = static_cast<u32>(Renderer::GetFrameData()->GetRenderFrameIndex())
                                 % MAX_FRAMES_IN_FLIGHT;
                VkDescriptorSet boneSet = BoneMatrixBuffer::GetDescriptorSet(slot);
                vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE,
                                        m_ComputePipeline->GetLayout(),
                                        0, 1, &boneSet, 0, nullptr);
                boundPipeline = true;
            }

            Dispatch(cmd, *mesh, inst.boneOffset);
        }
    }

    void SkinningSubsystem::AddDeformPass(RG::RenderGraph& rg)
    {
        struct DeformData {};
        rg.AddComputePass<DeformData>(
            "Deform",
            [](DeformData&, RG::RenderPassBuilder& builder) {
                // Per-mesh deformed buffers are persistent VMA allocations outside the RG; the hand-
                // rolled barrier below carries visibility to the raster vertex fetch (RG buffer-read
                // states have no VERTEX_SHADER variant). SetHasSideEffect keeps the pass uncullable.
                builder.SetHasSideEffect();
            },
            [this](DeformData&, RG::RenderPassContext& ctx) {
                VkCommandBuffer cmd = ctx.commandBuffer;
                auto* rs = SystemRegistry::GetSystem<RenderingSystem>();
                if (!rs) return;
                const RenderSnapshot& snapshot = rs->GetActiveSnapshot();

                // Skin every view (NOT multi-view-guarded): the deformed buffer is scene-global, but the
                // cross-view semaphore waits at EARLY_FRAGMENT_TESTS, which would not gate view 2's
                // VERTEX_SHADER fetch of a view-1-only write. Re-skinning per view is idempotent + cheap,
                // and view 2's gA already waits on view 1, so it adds no new serialization.
                DispatchAllSkinned(cmd, snapshot);

                // One global compute-write barrier over every mesh's deformed buffer. dst spans the
                // raster vertex fetch (VERTEX_SHADER) + fragment TBN reads, the BLAS-build vertex read,
                // and the rayQuery-in-compute trace reads. Same-queue raster (gA) is gated here; cross-
                // queue consumers (refit + RT on async-compute, GeometryPass on gB) ride the gA→compute
                // →gB timeline semaphores. see arch/multi-queue.md
                VkMemoryBarrier2 mem{ VK_STRUCTURE_TYPE_MEMORY_BARRIER_2 };
                mem.srcStageMask  = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
                mem.srcAccessMask = VK_ACCESS_2_SHADER_WRITE_BIT;
                mem.dstStageMask  = VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT
                                  | VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT
                                  | VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_BUILD_BIT_KHR
                                  | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
                mem.dstAccessMask = VK_ACCESS_2_SHADER_READ_BIT;
                VkDependencyInfo dep{ VK_STRUCTURE_TYPE_DEPENDENCY_INFO };
                dep.memoryBarrierCount = 1;
                dep.pMemoryBarriers    = &mem;
                vkCmdPipelineBarrier2(cmd, &dep);
            });
    }
}
