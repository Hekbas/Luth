#pragma once

#include "luth/core/LuthTypes.h"
#include "luth/renderer/rendergraph/RenderGraph.h"
#include "luth/renderer/rendergraph/RenderGraphSnapshot.h"
#include "luth/renderer/lighting/LightTypes.h"

#include <entt/entt.hpp>
#include <filesystem>
#include <memory>
#include <unordered_set>
#include <vector>

namespace Luth
{
    class Entity;
    class Material;
    class RenderingSystem;
    class Texture;
    struct GeometryOutput;
    struct SelectionMaskOutput;
    namespace fs = std::filesystem;

    // Owns the per-frame render-graph assembly and execution. Created by
    // RenderingSystem in its ctor and invoked once per frame from Update
    // after BuildGPUObjectBuffer / DrawListBuilder::Build have populated
    // the inputs.
    //
    // RenderPipeline is tightly coupled to RenderingSystem in v1 of this
    // split (sub-task D of epic arch-renderer-split): it reads rendering
    // resources (pipelines, descriptor sets, SPIR-V, samplers) through a
    // RenderingSystem& reference. Sub-task E migrates those resources
    // onto RenderPipeline itself.
    class RenderPipeline
    {
    public:
        explicit RenderPipeline(RenderingSystem& system);

        // One-time init: allocates all Vulkan pipeline resources (UBOs,
        // descriptor sets, samplers, SPIR-V, IBL maps, pipelines). Runs
        // when the Vulkan backend is active; no-op otherwise. Called from
        // RenderingSystem::ctor after FrameTargets has been allocated.
        void Initialize(u32 viewportWidth, u32 viewportHeight);

        // Tear down all Vulkan resources. Called from RenderingSystem::dtor.
        void Shutdown();

        // Build + execute the render graph for one frame.
        void Execute(entt::registry& registry);

        // Minimal graph (ImGui only). Used by the Frame Debugger Frozen state
        // when the camera hasn't moved — the LDR output still holds the last
        // captured image, so redrawing just the UI is enough to keep Dear ImGui
        // responsive without rebuilding the full graph.
        void ExecuteMinimal();

        // Recreate viewport-dependent resources (post-process descriptors,
        // GTAO storage textures, outline/grid descriptors). Called from
        // RenderingSystem::Resize after FrameTargets::Resize.
        void OnResize(u32 width, u32 height);

        // Reload the environment HDR → irradiance + prefiltered cubemaps + BRDF LUT.
        void ReloadSkybox(const fs::path& hdrPath);

        // Recompile all inline (non-AssetManager) shader SPIR-V (fullscreen,
        // bloom, post-process, outline, grid, selection, depth-prepass, skinned
        // variants) and rebuild their pipelines. Invoked by RenderingSystem's
        // shader hot-reload drain when a utility shader file changes on disk.
        void RecompileUtilityShaders();

        // Per-frame CPU-side GPU state prep. Called from RenderingSystem::Update
        // before the draw list is built and the graph executes.
        void UpdateGlobalUniforms();
        void UpdatePostProcessUBO();
        void UpdateGTAOUBO();
        void BuildGPUObjectBuffer(entt::registry& registry);
        u32  EnsureMaterialRegistered(std::shared_ptr<Material> material);

        // Editor + frame-debugger lookups (RenderingSystem forwards to these).
        std::shared_ptr<Texture> GetNamedTexture(const std::string& name) const;
        void ReplayPassUpToDraw(u32 passIdx, u32 localDrawIdx);
        void BlitArchivedDepthToPreview(u32 archiveIdx, int layer, float nearZ, float farZ);

    private:
        // Init / Update helpers (moved from RenderingSystem in sub-task E1).
        // All of these read/write RenderingSystem private fields via friend
        // access; field ownership migrates to RenderPipeline in sub-task E3.
        void InitGlobalUniforms();
        void InitShadowResources();
        void InitPostProcessResources();
        void UpdatePostProcessDescriptors();
        void InitIBLResources(const fs::path& hdrPath);
        void InitObjectSSBODescriptorLayout();
        void InitGPUObjectBuffers();
        void InitCullPipeline();
        void InitAOResources();
        void UpdateAODescriptors();
        void CreatePipelines();
        void RegisterNamedTextures();
        void InitDebugBlitResources();
        RG::ResourceHandle AddDebugBlitPass(RG::RenderGraph& rg, RG::ResourceHandle inputHandle, bool isDepth);
        void EnsurePerDrawPreviewTexture(u32 width, u32 height);
        void DestroyPerDrawPreviewTexture();
        void EnsureDepthPreviewTexture(u32 width, u32 height);
        void DestroyDepthPreviewTexture();

        // Render-graph pass builders. Each declares one RG pass (setup +
        // execute lambdas) and returns a handle to its primary output so
        // callers can chain the graph. All pass files live under
        // renderer/passes/ and used to be RenderingSystem methods.
        RG::ResourceHandle AddDepthPrepass(RG::RenderGraph& rg, entt::registry& registry, RG::BufferHandle indirectBufferHandle);
        RG::ResourceHandle AddGTAODepthPrefilterPass(RG::RenderGraph& rg, RG::ResourceHandle sceneDepth);
        RG::ResourceHandle AddGTAOMainPass(RG::RenderGraph& rg, RG::ResourceHandle linearDepth);
        RG::ResourceHandle AddGTAODenoisePass(RG::RenderGraph& rg, RG::ResourceHandle rawAO, RG::ResourceHandle linearDepth);
        RG::ResourceHandle AddShadowPass(RG::RenderGraph& rg, entt::registry& registry, RG::BufferHandle indirectBufferHandle, u32 cascadeIndex);
        GeometryOutput AddGeometryPass(RG::RenderGraph& rg, entt::registry& registry,
                                        const RG::ResourceHandle (&shadowHandles)[k_ShadowCascadeCount],
                                        RG::BufferHandle indirectBufferHandle,
                                        RG::ResourceHandle sceneDepth);
        RG::ResourceHandle AddSkyboxPass(RG::RenderGraph& rg, RG::ResourceHandle sceneColor, RG::ResourceHandle sceneDepth);
        RG::ResourceHandle AddBloomPasses(RG::RenderGraph& rg, RG::ResourceHandle sceneColor);
        RG::ResourceHandle AddPostProcessPass(RG::RenderGraph& rg, RG::ResourceHandle sceneColor, RG::ResourceHandle bloomResult);
        SelectionMaskOutput AddSelectionMaskPass(RG::RenderGraph& rg, entt::registry& registry);
        RG::ResourceHandle AddOutlinePass(RG::RenderGraph& rg, RG::ResourceHandle ldrOutput, SelectionMaskOutput maskOutput, RG::ResourceHandle sceneDepth);
        RG::ResourceHandle AddGridPass(RG::RenderGraph& rg, RG::ResourceHandle sceneColor, RG::ResourceHandle sceneDepth);
        void AddImGuiPass(RG::RenderGraph& rg, RG::ResourceHandle sceneColor);

        void CollectSelectedHandles(const std::vector<Entity>& selected, std::unordered_set<entt::entity>& outHandles) const;

        RG::RenderGraphSnapshot CaptureSnapshot(const RG::RenderGraph& rg);

        RenderingSystem& m_System;
    };
}
