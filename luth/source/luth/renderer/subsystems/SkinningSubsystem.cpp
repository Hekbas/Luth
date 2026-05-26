#include "luthpch.h"
#include "luth/renderer/subsystems/SkinningSubsystem.h"
#include "luth/renderer/RenderPipeline.h"
#include "luth/renderer/Renderer.h"
#include "luth/renderer/backend/vulkan/VulkanContext.h"
#include "luth/renderer/backend/vulkan/VulkanAccelerationStructure.h"
#include "luth/renderer/resources/BoneMatrixBuffer.h"
#include "luth/renderer/resources/Mesh.h"
#include "luth/renderer/shader/ShaderLibrary.h"
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

    void SkinningSubsystem::AddSkinningPass(RG::RenderGraph& /*rg*/, const RenderSnapshot& /*snapshot*/)
    {
        // Pass wiring lands with B.2.D's TlasBuildPass — both passes share the AsyncCompute
        // primary and a single ComputeWrite → AccelerationStructureBuild barrier covers the
        // deformed-buffer handoff. Stub kept here so the call site exists for B.2.D to flip on.
    }
}
