#include "luthpch.h"
#include "luth/renderer/subsystems/SkinningSubsystem.h"
#include "luth/renderer/RenderPipeline.h"
#include "luth/renderer/Renderer.h"
#include "luth/renderer/backend/vulkan/VulkanContext.h"
#include "luth/renderer/backend/vulkan/VulkanAccelerationStructure.h"
#include "luth/renderer/backend/vulkan/VulkanBuffer.h"
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
#include "luth/core/time/Time.h"
#include "luth/core/types/LuthMath.h"

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

        // deform.comp push constants — 72 B; the uint64 BDAs force 8-align (14 trailing 4-byte fields
        // → 72 is exactly 8-aligned, no pad). windXYZ is the per-instance OBJECT-space wind direction
        // (the global world-space dir transformed by inverse(mat3(world)) on the CPU).
        struct DeformPC
        {
            VkDeviceAddress inputBda;      //  0 — source Vertex VB (52 B)
            VkDeviceAddress deformedBda;   //  8 — write the CURRENT region
            u32             vertexCount;   // 16
            f32             time;          // 20
            f32             windX;         // 24 — object-space dir (CPU-resolved per instance)
            f32             windY;         // 28
            f32             windZ;         // 32
            f32             strength;      // 36
            f32             mainBendScale; // 40
            f32             detailScale;   // 44
            f32             frequency;     // 48
            f32             phaseOffset;   // 52 — per-entity de-sync
            f32             gustStrength;  // 56
            f32             gustFrequency; // 60
            f32             turbAmplitude; // 64
            f32             turbFrequency; // 68
        };
        static_assert(sizeof(DeformPC) == 72, "DeformPC must cover deform.comp's push range");

        constexpr u32 LOCAL_SIZE_X = 64;
    }

    void SkinningSubsystem::Init(RenderPipeline& pipeline)
    {
        LH_PROFILE_FUNCTION();
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

        // deform.comp — static wind-deformable. No bones / no descriptor set; all inputs ride DeformPC.
        if (auto sh = ShaderLibrary::LoadEngine("shaders/deform.comp"))
            m_DeformSpv = sh->GetSpirV();
        if (!m_DeformSpv.empty())
        {
            VkPushConstantRange dpc{ VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(DeformPC) };
            m_DeformPipeline = std::make_unique<VKComputePipeline>(
                m_DeformSpv, std::vector<VkDescriptorSetLayout>{}, std::vector<VkPushConstantRange>{ dpc });
        }
        else
            LH_CORE_ERROR("SkinningSubsystem: failed to load shaders/deform.comp");
    }

    void SkinningSubsystem::Shutdown()
    {
        LH_PROFILE_FUNCTION();
        m_ComputePipeline.reset();
        m_Spv.clear();
        m_DeformPipeline.reset();
        m_DeformSpv.clear();
        m_Pipeline = nullptr;
    }

    bool SkinningSubsystem::OnShaderReloaded(const std::string& name, const std::vector<u32>& spv)
    {
        LH_PROFILE_FUNCTION();
        if (name == "skinning.comp")
        {
            m_Spv = spv;
            VkPushConstantRange pcRange{ VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(SkinPC) };
            m_ComputePipeline = std::make_unique<VKComputePipeline>(
                m_Spv,
                std::vector<VkDescriptorSetLayout>{ BoneMatrixBuffer::GetDescriptorSetLayout() },
                std::vector<VkPushConstantRange>{ pcRange });
            LH_CORE_INFO("SkinningSubsystem: skinning.comp rebuilt after shader reload");
            return true;
        }
        if (name == "deform.comp")
        {
            m_DeformSpv = spv;
            VkPushConstantRange dpc{ VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(DeformPC) };
            m_DeformPipeline = std::make_unique<VKComputePipeline>(
                m_DeformSpv, std::vector<VkDescriptorSetLayout>{}, std::vector<VkPushConstantRange>{ dpc });
            LH_CORE_INFO("SkinningSubsystem: deform.comp rebuilt after shader reload");
            return true;
        }
        return false;
    }

    void SkinningSubsystem::Dispatch(VkCommandBuffer cmd, const Mesh& mesh, u32 boneOffset, u32 frameAbs) const
    {
        LH_PROFILE_FUNCTION();
        const auto& blas = mesh.GetBlas();
        if (!blas || !blas->IsDeformable()) return;
        if (blas->GetDeformedBdaCurr(frameAbs) == 0) return;

        // The compute reads the source SkinnedVertex VB directly (scalar buffer_reference) — its
        // upload fence is waited at BLAS-build time, so it is resident before the first dispatch.
        auto vb = std::dynamic_pointer_cast<VKVertexBuffer>(mesh.GetVertexBuffer());
        if (!vb) return;

        SkinPC pc{};
        pc.inputBda    = vb->GetDeviceAddress();
        pc.deformedBda = blas->GetDeformedBdaCurr(frameAbs);  // write the CURRENT region
        pc.vertexCount = blas->GetVertexCount();
        pc.boneOffset  = boneOffset;

        vkCmdPushConstants(cmd, m_ComputePipeline->GetLayout(),
                           VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(SkinPC), &pc);

        const u32 groups = (pc.vertexCount + LOCAL_SIZE_X - 1) / LOCAL_SIZE_X;
        vkCmdDispatch(cmd, groups, 1, 1);
    }

    void SkinningSubsystem::DispatchAllSkinned(VkCommandBuffer cmd, const RenderSnapshot& snapshot) const
    {
        LH_PROFILE_FUNCTION();
        if (!m_ComputePipeline) return;

        const u32 frameAbs = static_cast<u32>(Renderer::GetFrameData()->GetRenderFrameIndex());
        bool boundPipeline = false;
        for (const auto& inst : snapshot.meshes)
        {
            if (!inst.isSkinned) continue;
            auto model = Luth::AssetManager::GetAsset<Model>(inst.modelUUID);
            if (!model) continue;
            auto mesh = model->GetMesh(inst.meshIndex);
            if (!mesh) continue;
            const auto& blas = mesh->GetBlas();
            if (!blas || !blas->IsDeformable()) continue;

            // Lazy bind so a snapshot with zero skinned meshes records zero compute commands.
            if (!boundPipeline)
            {
                m_ComputePipeline->Bind(cmd);
                const u32 slot = frameAbs % MAX_FRAMES_IN_FLIGHT;
                VkDescriptorSet boneSet = BoneMatrixBuffer::GetDescriptorSet(slot);
                vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE,
                                        m_ComputePipeline->GetLayout(),
                                        0, 1, &boneSet, 0, nullptr);
                boundPipeline = true;
            }

            Dispatch(cmd, *mesh, inst.boneOffset, frameAbs);
        }
    }

    void SkinningSubsystem::DispatchAllDeformable(VkCommandBuffer cmd, const RenderSnapshot& snapshot,
                                                  const WindSettings& wind, f32 time) const
    {
        LH_PROFILE_FUNCTION();
        if (!m_DeformPipeline) return;

        const u32 frameAbs = static_cast<u32>(Renderer::GetFrameData()->GetRenderFrameIndex());
        // Global wind FIELD. The world-space direction is transformed into each mesh's object space
        // inside the loop (so rotated instances bend the same world direction); a zero vector → no main
        // bend (detail still applies). Per-entity response (Component::Wind) folds in below.
        const f32  wlen      = Math::Length(wind.direction);
        const Vec3 worldDir  = (wlen > 1e-5f) ? wind.direction * (1.0f / wlen) : Vec3(0.0f);
        const f32  gStrength = wind.enabled ? wind.strength : 0.0f;

        bool boundPipeline = false;
        for (const auto& inst : snapshot.meshes)
        {
            if (!inst.isDeformable || inst.isSkinned) continue;   // skinned deforms via skinning.comp
            auto model = Luth::AssetManager::GetAsset<Model>(inst.modelUUID);
            if (!model) continue;
            auto mesh = model->GetMesh(inst.meshIndex);
            if (!mesh) continue;
            const auto& blas = mesh->GetBlas();
            if (!blas || !blas->IsDeformable() || blas->GetDeformedBdaCurr(frameAbs) == 0) continue;
            auto vb = std::dynamic_pointer_cast<VKVertexBuffer>(mesh->GetVertexBuffer());
            if (!vb) continue;

            // Lazy bind — no descriptor set (deform.comp has no Set 0). A snapshot with zero deformable
            // meshes records zero commands.
            if (!boundPipeline) { m_DeformPipeline->Bind(cmd); boundPipeline = true; }

            // World→object: a wind direction is a contravariant flow vector, so transform by the plain
            // inverse of the linear part (NOT the inverse-transpose normal matrix). Singular → no bend.
            const Mat3 invLin = Math::Inverse(Mat3(inst.worldMatrix));

            // Per-entity direction: an override (world- or object-space) else the global world field.
            // World-space sources are pulled into the mesh's object space; an object-space override rides raw.
            Vec3 objDir = (inst.windDirOverride && !inst.windOverrideIsWorld)
                ? inst.windOverrideDir
                : invLin * (inst.windDirOverride ? inst.windOverrideDir : worldDir);
            // Upper bound rejects inf from a singular (zero-scale) model matrix; nan fails both compares.
            const f32 olen = Math::Length(objDir);
            objDir = (olen > 1e-5f && olen < 1e18f) ? objDir * (1.0f / olen) : Vec3(0.0f);

            // Per-entity response folds into the global field; windRespond == false → bind pose.
            const f32 strength = inst.windRespond ? (gStrength * inst.windStrengthMul) : 0.0f;

            DeformPC pc{};
            pc.inputBda      = vb->GetDeviceAddress();             // source Vertex VB (52 B)
            pc.deformedBda   = blas->GetDeformedBdaCurr(frameAbs); // write the CURRENT region
            pc.vertexCount   = blas->GetVertexCount();
            pc.time          = time;
            pc.windX         = objDir.x;
            pc.windY         = objDir.y;
            pc.windZ         = objDir.z;
            pc.strength      = strength;
            pc.mainBendScale = wind.mainBendScale;
            pc.detailScale   = wind.detailScale * inst.windDetailMul;
            pc.frequency     = wind.frequency;
            pc.phaseOffset   = inst.windPhaseOffset;
            pc.gustStrength  = wind.gustStrength * inst.windGustMul;
            pc.gustFrequency = wind.gustFrequency;
            pc.turbAmplitude = wind.turbulenceAmplitude * inst.windDetailMul;
            pc.turbFrequency = wind.turbulenceFrequency;

            vkCmdPushConstants(cmd, m_DeformPipeline->GetLayout(),
                               VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(DeformPC), &pc);
            const u32 groups = (pc.vertexCount + LOCAL_SIZE_X - 1) / LOCAL_SIZE_X;
            vkCmdDispatch(cmd, groups, 1, 1);
        }
    }

    void SkinningSubsystem::AddDeformPass(RG::RenderGraph& rg)
    {
        LH_PROFILE_FUNCTION();
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
                DispatchAllDeformable(cmd, snapshot, rs->GetWindSettings(), Time::GetTime());

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
