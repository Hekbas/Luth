#pragma once

#include "luth/core/types/LuthTypes.h"
#include "luth/renderer/rendergraph/RenderGraph.h"
#include "luth/renderer/backend/vulkan/VulkanComputePipeline.h"
#include "luth/renderer/backend/vulkan/VulkanAllocator.h"

#include <memory>
#include <string>
#include <vector>
#include <vulkan/vulkan.h>

namespace Luth
{
    class RenderPipeline;
    class VKTexture;

    // Bindless-SPIR-V regression guard. The gate is CheckSlangSpirv() — a deterministic scan of the
    // compiled slang_spike_gi.slang for the NonUniform decorations + bindless/rayQuery/BDA capabilities
    // that slang#10525-class regressions break (run at init + on .slang reload). The runtime A/B is a
    // default-OFF visual diagnostic: it builds TWO compute pipelines from the SAME layouts + push constant
    // (slang_spike_gi.comp via libshaderc, slang_spike_gi.slang via Slang), dispatches both into RGBA32F
    // images + a diff reducer, and reads the result back — but a ~1-ULP camera-ray difference flips
    // silhouette hits (flickers under TAA jitter), so the pixel diff is informational, never a verdict.
    // One RG compute pass after the GI passes; SetHasSideEffect keeps it past the culler. Mirrors
    // RtRestirGiSubsystem's 5-set wiring.
    class SlangParityGuard
    {
    public:
        void Init(RenderPipeline& pipeline);
        void Shutdown();

        bool OnShaderReloaded(const std::string& name, const std::vector<u32>& spv);

        // One AsyncCompute pass: GLSL dispatch → Slang dispatch → diff reduce. Run once per frame
        // (multi-view guard); no-op when disabled, before a TLAS exists, or if Slang failed to compile.
        void AddPass(RG::RenderGraph& rg);

        // Backed by SlangParitySettings::enabled on the RenderingSystem; also gates the TLAS build.
        bool IsEnabled() const;

    private:
        // Lazy one-time setup: load the GLSL + Slang + diff shaders through ShaderLibrary, build the
        // pipelines, create layouts/pool/sets. Deferred to the first enabled AddPass so a disabled guard
        // costs nothing at runtime. Returns false (and logs once) on Slang failure.
        bool EnsureInitialized();
        void CheckSlangSpirv();   // deterministic SPIR-V regression gate — scans m_SlangSpv, sets the verdict
        void EnsureResources(u32 width, u32 height);
        void DestroyResources();
        void ReadbackDiff();   // map the host-visible diff buffer into the settings readout (1 frame stale)

        RenderPipeline* m_Pipeline = nullptr;

        std::unique_ptr<VKComputePipeline> m_GlslPipeline;   // slang_spike_gi.comp  (libshaderc)
        std::unique_ptr<VKComputePipeline> m_SlangPipeline;  // slang_spike_gi.slang (in-process Slang)
        std::unique_ptr<VKComputePipeline> m_DiffPipeline;   // slang_spike_diff.comp

        VkDescriptorSetLayout m_OutSetLayout  = VK_NULL_HANDLE;   // Set 2: b0 output storage image
        VkDescriptorSetLayout m_DiffSetLayout = VK_NULL_HANDLE;   // Set 0: b0/b1 in images + b2 diff SSBO
        VkDescriptorPool      m_Pool          = VK_NULL_HANDLE;
        VkDescriptorSet       m_GlslSet       = VK_NULL_HANDLE;   // m_OutSetLayout, binds m_ImgGlsl
        VkDescriptorSet       m_SlangSet      = VK_NULL_HANDLE;   // m_OutSetLayout, binds m_ImgSlang
        VkDescriptorSet       m_DiffSet       = VK_NULL_HANDLE;   // m_DiffSetLayout

        std::shared_ptr<VKTexture> m_ImgGlsl;    // RGBA32F storage, GENERAL
        std::shared_ptr<VKTexture> m_ImgSlang;
        VkBuffer      m_DiffBuf   = VK_NULL_HANDLE;   // 16 B host-visible verdict block
        VmaAllocation m_DiffAlloc = nullptr;

        std::vector<u32> m_GlslSpv;
        std::vector<u32> m_SlangSpv;
        std::vector<u32> m_DiffSpv;
        bool m_Initialized = false;   // EnsureInitialized ran (success or failure)
        bool m_InitOk      = false;   // in-process Slang compile + pipeline build succeeded

        u32 m_Width = 0, m_Height = 0;
        u64 m_LastRunFrame = ~0ull;   // multi-view guard — A/B runs on the first view only
    };
}
