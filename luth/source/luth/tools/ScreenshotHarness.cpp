#include "luthpch.h"
#include "luth/tools/ScreenshotHarness.h"

#include "luth/scene/Scene.h"
#include "luth/scene/Components.h"
#include "luth/scene/systems/RenderingSystem.h"
#include "luth/scene/systems/SystemRegistry.h"
#include "luth/renderer/RenderPipeline.h"
#include "luth/renderer/Renderer.h"
#include "luth/renderer/CameraParams.h"
#include "luth/renderer/backend/vulkan/VulkanContext.h"
#include "luth/renderer/backend/vulkan/VulkanTexture.h"
#include "luth/renderer/backend/vulkan/VulkanAllocator.h"
#include "luth/resources/Image.h"
#include "luth/resources/AssetManager.h"
#include "luth/renderer/resources/Model.h"

#include <vma/vk_mem_alloc.h>

namespace Luth
{
    using namespace Component;

    // World-space bounds of everything drawable, via the same bind-pose AABB lookup DebugGizmos::DrawAABBs
    // uses. Corners are transformed individually because a rotated model's world AABB is not the transform
    // of its local one.
    static AABB ComputeSceneBounds(entt::registry& reg)
    {
        AABB bounds;
        for (auto e : reg.view<WorldTransform, MeshRenderer>())
        {
            auto& wt = reg.get<WorldTransform>(e);
            auto& mr = reg.get<MeshRenderer>(e);

            auto model = AssetManager::GetAsset<Model>(mr.ModelUUID);
            if (!model) continue;
            auto& meshes = model->GetMeshesData();
            if (mr.MeshIndex >= meshes.size()) continue;

            const AABB& local = meshes[mr.MeshIndex].BindPoseAABB;
            if (!local.IsValid()) continue;

            for (int c = 0; c < 8; ++c) {
                const Vec3 corner{
                    (c & 1) ? local.Max.x : local.Min.x,
                    (c & 2) ? local.Max.y : local.Min.y,
                    (c & 4) ? local.Max.z : local.Min.z };
                const Vec4 world = wt.Matrix * Vec4(corner, 1.0f);
                bounds.Expand(Vec3(world));
            }
        }
        return bounds;
    }

    // Fallback for scenes authored against the editor camera, which is most of them: no Camera entity
    // exists, so frame the scene's bounds from a three-quarter view instead of rendering an empty sky.
    bool ScreenshotHarness::AutoFrame(CameraParams& out)
    {
        Scene* scene = SystemRegistry::GetScene();
        if (!scene) return false;

        const AABB bounds = ComputeSceneBounds(scene->Registry());
        if (!bounds.IsValid()) return false;

        // Streaming keeps growing the bounds, so restart the warmup whenever the framing moves: TAA and
        // ReSTIR would otherwise be converging against a camera that is still being decided.
        const bool moved = !m_LastBounds.IsValid()
                        || Math::Length(bounds.Min - m_LastBounds.Min) > 0.001f
                        || Math::Length(bounds.Max - m_LastBounds.Max) > 0.001f;
        if (moved) {
            m_LastBounds = bounds;
            m_Rendered   = 0;
        }

        const Vec3  center = bounds.Center();
        const float radius = Math::Max(Math::Length(bounds.Extents()), 0.001f);

        const float fovY = Math::Radians(45.0f);
        const float aspect = static_cast<float>(m_Spec.width) / static_cast<float>(m_Spec.height);
        // Fit by the tighter of the two axes so wide scenes are not clipped at the sides.
        const float fovFit = (aspect < 1.0f) ? 2.0f * Math::Atan(Math::Tan(fovY * 0.5f) * aspect) : fovY;
        const float dist   = (radius / Math::Sin(fovFit * 0.5f)) * 1.25f;

        const Vec3 dir = Math::Normalize(Vec3(1.0f, 0.6f, 1.0f));
        const Vec3 eye = center + dir * dist;

        out.view       = Math::LookAt(eye, center, Vec3(0.0f, 1.0f, 0.0f));
        out.projection = Math::Perspective(fovY, aspect, dist * 0.01f, dist + radius * 4.0f);
        out.position   = eye;
        out.nearZ      = dist * 0.01f;
        out.farZ       = dist + radius * 4.0f;

        if (moved)
            LH_CORE_INFO("ScreenshotHarness: no Camera entity; auto-framing scene bounds (radius {:.2f})", radius);
        return true;
    }

    // Mirrors GamePanel::BuildCameraFromScene. Two non-obvious constraints:
    //   - the projection stays Y-up; GlobalSubsystem::UpdateUBO applies the Vulkan Y-flip for every view,
    //     so flipping here would cancel it out and render the scene upside down.
    //   - Camera::ViewMatrix / ProjectionMatrix are never refreshed (CameraSystem is registered but never
    //     ticked), so the matrices must be rebuilt from WorldTransform rather than read off the component.
    bool ScreenshotHarness::BuildCamera(CameraParams& out)
    {
        Scene* scene = SystemRegistry::GetScene();
        if (!scene) return false;

        auto view = scene->Registry().view<Camera, WorldTransform>();
        auto it = view.begin();
        if (it == view.end()) return AutoFrame(out);

        auto& cam = view.get<Camera>(*it);
        auto& xf  = view.get<WorldTransform>(*it);

        // Capture aspect wins over the component's authored value: the output PNG is what we asked for.
        const float aspect = static_cast<float>(m_Spec.width) / static_cast<float>(m_Spec.height);

        out.view = Math::Inverse(xf.Matrix);
        if (cam.Projection == Camera::ProjectionType::Perspective) {
            out.projection = Math::Perspective(Math::Radians(cam.VerticalFOV), aspect, cam.NearClip, cam.FarClip);
            out.nearZ = cam.NearClip;
            out.farZ  = cam.FarClip;
        } else {
            const float halfH = cam.OrthographicSize * 0.5f;
            const float halfW = halfH * aspect;
            out.projection = Math::Ortho(-halfW, halfW, -halfH, halfH, cam.OrthographicNear, cam.OrthographicFar);
            out.nearZ = cam.OrthographicNear;
            out.farZ  = cam.OrthographicFar;
        }
        out.position = Vec3(xf.Matrix[3][0], xf.Matrix[3][1], xf.Matrix[3][2]);
        return true;
    }

    bool ScreenshotHarness::ResolveCamera(CameraParams& out)
    {
        if (m_Failed) return false;

        if (!BuildCamera(out)) {
            // Expected while assets are still streaming; only a scene that never resolves is an error.
            if (++m_WaitFrames > k_MaxWaitFrames) {
                LH_CORE_ERROR("ScreenshotHarness: no Camera entity and no drawable geometry after {} frames",
                              k_MaxWaitFrames);
                m_Failed = true;
            }
            return false;
        }

        // CameraParams defaults volumetric fog on, and the engine's default density swallows a scene
        // framed from any distance. The editor hides this because its sliders start elsewhere.
        out.enableVolumetricFog = false;

        if (!m_Resolved) {
            if (auto* rs = SystemRegistry::GetSystem<RenderingSystem>()) {
                // Editor furniture would otherwise land in the output.
                rs->SetGridVisible(false);
                // RenderingSystem is constructed with a hardcoded 1280x720 default and only ever resized
                // by the editor's viewport, so a headless run must ask for the capture size itself.
                rs->Resize(m_Spec.width, m_Spec.height);
            }
            m_Resolved = true;
        }
        return true;
    }

    bool ScreenshotHarness::CaptureIfReady()
    {
        if (m_Failed) return true;
        // Only frames that actually rendered the resolved camera count toward warmup.
        if (!m_Resolved) return false;

        // Textures stream in long after the meshes they belong to, and a mesh with no albedo yet reads as
        // blown-out white. Hold the warmup at zero until nothing is in flight, so --frames stays a pure
        // TAA/ReSTIR convergence budget rather than doubling as an asset-loading guess.
        if (AssetManager::AnyLoading()) {
            m_Rendered = 0;
            return false;
        }

        // Warmup lets TAA, ReSTIR and the volumetric resolve accumulate; a frame-0 capture is pure noise.
        if (++m_Rendered <= m_Spec.warmupFrames) return false;

        const bool ok = WritePng();
        m_Failed = !ok;
        return true;
    }

    bool ScreenshotHarness::WritePng()
    {
        LH_PROFILE_FUNCTION();

        auto* rs = SystemRegistry::GetSystem<RenderingSystem>();
        if (!rs) return false;

        const auto& ldr = rs->GetSceneTargets().GetLDROutput();
        if (!ldr) {
            LH_CORE_ERROR("ScreenshotHarness: no LDR target to capture");
            return false;
        }

        auto vkLdr = std::static_pointer_cast<VKTexture>(ldr);
        const u32 w = ldr->GetWidth();
        const u32 h = ldr->GetHeight();
        const VkDeviceSize bytes = static_cast<VkDeviceSize>(w) * h * 4;

        // One-shot staging for a teardown capture, so this stays off the tagged heap by design; same shape
        // as PickingSystem's readback. see arch/memory.md for why per-frame data does not do this.
        VkBuffer stagingBuf = VK_NULL_HANDLE;
        VkBufferCreateInfo bufInfo{ VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
        bufInfo.size  = bytes;
        bufInfo.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
        VmaAllocation stagingAlloc = VulkanAllocator::AllocateBuffer(bufInfo, VMA_MEMORY_USAGE_GPU_TO_CPU, stagingBuf);
        if (!stagingAlloc) {
            LH_CORE_ERROR("ScreenshotHarness: staging allocation failed ({} bytes)", bytes);
            return false;
        }

        VulkanContext::Get().ImmediateSubmit([&](VkCommandBuffer cmd)
        {
            // RenderPipeline::Execute leaves a non-primary view's LDR target in SHADER_READ_ONLY_OPTIMAL
            // so the editor can sample it; that is the layout we transition out of here.
            VkImageMemoryBarrier2 barrier{ VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2 };
            barrier.srcStageMask  = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
            barrier.srcAccessMask = VK_ACCESS_2_SHADER_READ_BIT;
            barrier.dstStageMask  = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
            barrier.dstAccessMask = VK_ACCESS_2_TRANSFER_READ_BIT;
            barrier.oldLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
            barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            barrier.image = vkLdr->GetImage();
            barrier.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };

            VkDependencyInfo dep{ VK_STRUCTURE_TYPE_DEPENDENCY_INFO };
            dep.imageMemoryBarrierCount = 1;
            dep.pImageMemoryBarriers = &barrier;
            vkCmdPipelineBarrier2(cmd, &dep);

            VkBufferImageCopy region{};
            region.bufferOffset = 0;
            region.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
            region.imageExtent = { w, h, 1 };
            vkCmdCopyImageToBuffer(cmd, vkLdr->GetImage(), VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                                   stagingBuf, 1, &region);

            // Hand the layout back exactly as found. The render graph's barrier solver does not know this
            // copy happened, so a leaked layout would desync every subsequent frame's barriers.
            std::swap(barrier.oldLayout, barrier.newLayout);
            barrier.srcStageMask  = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
            barrier.srcAccessMask = VK_ACCESS_2_TRANSFER_READ_BIT;
            barrier.dstStageMask  = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
            barrier.dstAccessMask = VK_ACCESS_2_SHADER_READ_BIT;
            vkCmdPipelineBarrier2(cmd, &dep);
        });

        void* mapped = VulkanAllocator::Map(stagingAlloc);
        bool ok = false;
        if (mapped) {
            const auto parent = m_Spec.shotPath.parent_path();
            if (!parent.empty()) {
                std::error_code ec;
                std::filesystem::create_directories(parent, ec);
            }
            ok = Image::SavePng(m_Spec.shotPath, static_cast<const u8*>(mapped), w, h, 4);
            VulkanAllocator::Unmap(stagingAlloc);
        }
        VulkanAllocator::FreeBuffer(stagingBuf, stagingAlloc);

        if (ok) LH_CORE_INFO("ScreenshotHarness: wrote {}x{} capture to '{}'", w, h, m_Spec.shotPath.string());
        else    LH_CORE_ERROR("ScreenshotHarness: failed to write '{}'", m_Spec.shotPath.string());
        return ok;
    }
}
