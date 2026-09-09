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
#include "luth/renderer/FrameTargets.h"
#include "luth/tools/ImageMetrics.h"
#include <cstring>

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

    // An empty scene has no bounds to frame, which is a hard error for a screenshot but a legitimate
    // request for a benchmark: the fixed per-frame cost of this pipeline is what an empty-scene run is
    // measuring. Park the camera at the origin looking down -Z so the view is well-formed.
    bool ScreenshotHarness::DefaultCamera(CameraParams& out)
    {
        const float aspect = static_cast<float>(m_Spec.width) / static_cast<float>(m_Spec.height);
        out.position   = Vec3(0.0f, 1.0f, 5.0f);
        out.view       = Math::LookAt(out.position, Vec3(0.0f, 1.0f, 0.0f), Vec3(0.0f, 1.0f, 0.0f));
        out.projection = Math::Perspective(Math::Radians(45.0f), aspect, 0.1f, 1000.0f);
        out.nearZ      = 0.1f;
        out.farZ       = 1000.0f;
        return true;
    }

    bool ScreenshotHarness::ResolveCamera(CameraParams& out)
    {
        if (m_Failed) return false;

        if (!BuildCamera(out)) {
            // Expected while assets are still streaming; only a scene that never resolves is an error.
            if (++m_WaitFrames > k_MaxWaitFrames) {
                if (m_Spec.WantsBenchmark() && !m_Spec.WantsCapture()) {
                    if (!m_Resolved)
                        LH_CORE_INFO("ScreenshotHarness: empty scene; benchmarking with the default camera");
                    DefaultCamera(out);
                }
                else {
                    LH_CORE_ERROR("ScreenshotHarness: no Camera entity and no drawable geometry after {} frames",
                                  k_MaxWaitFrames);
                    m_Failed = true;
                    return false;
                }
            }
            else return false;
        }

        // Off by default because the stock fog density swallows a scene framed from any distance (the
        // editor hides this because its sliders start elsewhere), but no longer forced: --fog on honors
        // the saved setting, so a capture can reproduce the viewport when that is what is being checked.
        out.enableVolumetricFog = m_Spec.fog && out.enableVolumetricFog;

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

        // --spp reads the path tracer's own accumulator instead of counting frames, so a run cannot be
        // cut short by samplesPerFrame > 1 and an accumulation reset cannot pass for progress.
        if (m_Spec.spp > 0)
        {
            auto* rsPt = SystemRegistry::GetSystem<RenderingSystem>();
            if (rsPt && rsPt->GetPipeline().GetPathTrace().GetLastSampleCount() < m_Spec.spp)
                return false;
        }

        bool ok = true;
        // HDR first: it fills the linear buffer the scoring reads, and it must run whenever a reference
        // was given even if no --hdr file was requested.
        if (!m_Spec.hdrPath.empty() || m_Spec.WantsMetrics()) ok = WriteHdr() && ok;
        if (!m_Spec.shotPath.empty())                         ok = WritePng() && ok;
        if (m_Spec.WantsMetrics())                            ok = RunMetrics() && ok;

        m_Failed = !ok;
        return true;
    }

    // Half-to-float for the RGBA16F scene colour. Handles subnormals and inf/nan rather than taking the
    // usual fast path, because an inf reaching the metrics would silently poison every number.
    static f32 HalfToFloat(u16 h)
    {
        const u32 sign = u32(h & 0x8000u) << 16;
        const u32 exp  = (h >> 10) & 0x1Fu;
        const u32 mant = h & 0x3FFu;
        u32 out;
        if (exp == 0u)
        {
            if (mant == 0u) out = sign;                     // signed zero
            else
            {   // subnormal: renormalise into a float exponent
                u32 e = 0u, m = mant;
                while ((m & 0x400u) == 0u) { m <<= 1; ++e; }
                m &= 0x3FFu;
                out = sign | ((127u - 15u - e) << 23) | (m << 13);
            }
        }
        else if (exp == 31u) out = sign | 0x7F800000u | (mant << 13);   // inf / nan
        else                 out = sign | ((exp + 127u - 15u) << 23) | (mant << 13);
        f32 f; std::memcpy(&f, &out, sizeof(f));
        return f;
    }

    bool ScreenshotHarness::Readback(const std::shared_ptr<Texture>& tex, u32 bytesPerPixel,
                                     VkImageLayout srcLayout, std::vector<u8>& out, u32& outW, u32& outH)
    {
        LH_PROFILE_FUNCTION();
        if (!tex) return false;

        auto vkTex = std::static_pointer_cast<VKTexture>(tex);
        const u32 w = tex->GetWidth();
        const u32 h = tex->GetHeight();
        outW = w; outH = h;
        const VkDeviceSize bytes = VkDeviceSize(w) * h * bytesPerPixel;

        // One-shot staging for a teardown capture, so this stays off the tagged heap by design; same
        // shape as PickingSystem's readback. see arch/memory.md for why per-frame data does not do this.
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
            // The LDR target ends SHADER_READ_ONLY_OPTIMAL so the editor can sample it, and sceneColor
            // likewise because PostProcess is its last reader; the path tracer's accumulator is a storage
            // image and ends GENERAL. Compute stage on the source covers the storage case.
            const bool storage = (srcLayout == VK_IMAGE_LAYOUT_GENERAL);
            VkImageMemoryBarrier2 barrier{ VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2 };
            barrier.srcStageMask  = storage ? VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT
                                            : VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
            barrier.srcAccessMask = storage ? VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT
                                            : VK_ACCESS_2_SHADER_READ_BIT;
            barrier.dstStageMask  = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
            barrier.dstAccessMask = VK_ACCESS_2_TRANSFER_READ_BIT;
            barrier.oldLayout = srcLayout;
            barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
            barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            barrier.image = vkTex->GetImage();
            barrier.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };

            VkDependencyInfo dep{ VK_STRUCTURE_TYPE_DEPENDENCY_INFO };
            dep.imageMemoryBarrierCount = 1;
            dep.pImageMemoryBarriers = &barrier;
            vkCmdPipelineBarrier2(cmd, &dep);

            VkBufferImageCopy region{};
            region.bufferOffset = 0;
            region.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
            region.imageExtent = { w, h, 1 };
            vkCmdCopyImageToBuffer(cmd, vkTex->GetImage(), VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                                   stagingBuf, 1, &region);

            // Hand the layout back exactly as found. The render graph's barrier solver does not know this
            // copy happened, so a leaked layout would desync every subsequent frame's barriers.
            std::swap(barrier.oldLayout, barrier.newLayout);
            barrier.srcStageMask  = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
            barrier.srcAccessMask = VK_ACCESS_2_TRANSFER_READ_BIT;
            barrier.dstStageMask  = storage ? VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT
                                            : VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
            barrier.dstAccessMask = storage ? VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT
                                            : VK_ACCESS_2_SHADER_READ_BIT;
            vkCmdPipelineBarrier2(cmd, &dep);
        });

        bool ok = false;
        if (void* mapped = VulkanAllocator::Map(stagingAlloc))
        {
            out.resize(static_cast<size_t>(bytes));
            std::memcpy(out.data(), mapped, static_cast<size_t>(bytes));
            VulkanAllocator::Unmap(stagingAlloc);
            ok = true;
        }
        VulkanAllocator::FreeBuffer(stagingBuf, stagingAlloc);
        return ok;
    }

    bool ScreenshotHarness::WritePng()
    {
        LH_PROFILE_FUNCTION();
        auto* rs = SystemRegistry::GetSystem<RenderingSystem>();
        if (!rs) return false;

        const auto& ldr = rs->GetSceneTargets().GetLDROutput();
        if (!ldr) { LH_CORE_ERROR("ScreenshotHarness: no LDR target to capture"); return false; }

        std::vector<u8> bytes; u32 w = 0, h = 0;
        if (!Readback(ldr, 4, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, bytes, w, h)) return false;

        const auto parent = m_Spec.shotPath.parent_path();
        if (!parent.empty()) { std::error_code ec; std::filesystem::create_directories(parent, ec); }

        const bool ok = Image::SavePng(m_Spec.shotPath, bytes.data(), w, h, 4);
        if (ok) LH_CORE_INFO("ScreenshotHarness: wrote {}x{} capture to '{}'", w, h, m_Spec.shotPath.string());
        else    LH_CORE_ERROR("ScreenshotHarness: failed to write '{}'", m_Spec.shotPath.string());
        return ok;
    }

    bool ScreenshotHarness::WriteHdr()
    {
        LH_PROFILE_FUNCTION();
        auto* rs = SystemRegistry::GetSystem<RenderingSystem>();
        if (!rs) return false;

        // PathTrace mode SKIPS the realtime pipeline, so sceneColor is never written and reading it
        // yields an all-black reference (caught by the lumRatio sanity line: refMeanLum == 0). Take the
        // path tracer's own accumulator instead, which is better anyway: RGBA32F holding the running
        // MEAN, so it needs no sample-count normalisation and no fp16 rounding. see RenderPipeline.h
        std::vector<u8> raw; u32 w = 0, h = 0;
        const bool pathTraced = (rs->GetRenderMode() == RenderMode::PathTrace);

        if (pathTraced)
        {
            const ViewResources* vr = rs->GetPipeline().GetCurrentViewResources();
            if (!vr || !vr->ptAccum) {
                LH_CORE_ERROR("ScreenshotHarness: PathTrace mode but no accumulator to capture");
                return false;
            }
            if (!Readback(vr->ptAccum, 16, VK_IMAGE_LAYOUT_GENERAL, raw, w, h)) return false;
            m_HdrLinear.resize(static_cast<size_t>(w) * h * 4);
            std::memcpy(m_HdrLinear.data(), raw.data(), raw.size());   // already RGBA32F
        }
        else
        {
            // Pre-tonemap linear radiance: the only version of the image where a firefly is still a
            // number rather than clipped white. see tools/ImageMetrics.h
            const auto& hdr = rs->GetSceneTargets().GetSceneColor();
            if (!hdr) { LH_CORE_ERROR("ScreenshotHarness: no HDR target to capture"); return false; }
            if (!Readback(hdr, 8, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, raw, w, h)) return false;

            const size_t comps = static_cast<size_t>(w) * h * 4;
            m_HdrLinear.resize(comps);
            const u16* src = reinterpret_cast<const u16*>(raw.data());
            for (size_t i = 0; i < comps; ++i) m_HdrLinear[i] = HalfToFloat(src[i]);
        }
        m_HdrW = w; m_HdrH = h;

        if (m_Spec.hdrPath.empty()) return true;   // scoring only; no file was asked for

        const auto parent = m_Spec.hdrPath.parent_path();
        if (!parent.empty()) { std::error_code ec; std::filesystem::create_directories(parent, ec); }

        const bool ok = Image::SaveHDR(m_Spec.hdrPath, m_HdrLinear.data(), w, h, 4);
        if (ok) LH_CORE_INFO("ScreenshotHarness: wrote {}x{} linear HDR to '{}'", w, h, m_Spec.hdrPath.string());
        else    LH_CORE_ERROR("ScreenshotHarness: failed to write '{}'", m_Spec.hdrPath.string());
        return ok;
    }

    bool ScreenshotHarness::RunMetrics()
    {
        LH_PROFILE_FUNCTION();
        if (m_HdrLinear.empty()) { LH_CORE_ERROR("ScreenshotHarness: no HDR buffer to score"); return false; }

        Image::LoadResultF ref = Image::LoadHDR(m_Spec.comparePath);
        if (!ref.valid) {
            LH_CORE_ERROR("ScreenshotHarness: cannot load reference '{}'", m_Spec.comparePath.string());
            return false;
        }

        const u32 w = m_HdrW, h = m_HdrH;
        if (ref.width != w || ref.height != h) {
            // Rescaling here would make every number a measure of the resampler, so refuse instead.
            LH_CORE_ERROR("ScreenshotHarness: reference is {}x{} but capture is {}x{}; re-bake the "
                          "reference at the capture size", ref.width, ref.height, w, h);
            return false;
        }

        const ImageMetrics::Result r = ImageMetrics::Compare(m_HdrLinear.data(), ref.pixels.data(), w, h);
        LH_CORE_INFO("ScreenshotHarness: relMSE {:.6f}  p99 {:.4f}  p99.9 {:.4f}  SSIM {:.4f}  lumRatio {:.3f}",
                     r.relMSE, r.p99RelErr, r.p999RelErr, r.ssim,
                     (r.refMeanLum > 0.0) ? r.testMeanLum / r.refMeanLum : 0.0);

        bool ok = true;
        if (!m_Spec.metricsPath.empty())
            ok = ImageMetrics::WriteJson(m_Spec.metricsPath, r, m_Spec.hdrPath, m_Spec.comparePath) && ok;
        if (!m_Spec.errorMapPath.empty())
            ok = ImageMetrics::WriteErrorMap(m_Spec.errorMapPath, m_HdrLinear.data(),
                                             ref.pixels.data(), w, h) && ok;
        return ok;
    }
}
