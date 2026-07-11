#pragma once

#include "luth/scene/systems/ISystem.h"
#include "luth/scene/Entity.h"
#include "luth/memory/Memory.h"
#include "luth/renderer/CameraParams.h"
#include "luth/renderer/DrawListBuilder.h"
#include "luth/renderer/FrameDebugger.h"
#include "luth/renderer/FrameTargets.h"
#include "luth/renderer/RenderPipeline.h"
#include "luth/renderer/draw/DrawList.h"
#include "luth/renderer/rendergraph/RenderGraph.h"
#include "luth/renderer/rendergraph/RenderGraphSnapshot.h"
#include "luth/renderer/rendergraph/FrameCapture.h"
#include "luth/renderer/lighting/LightTypes.h"
#include "luth/renderer/settings/PostProcessSettings.h"
#include "luth/renderer/settings/VolumetricSettings.h"
#include "luth/renderer/settings/RestirSettings.h"
#include "luth/renderer/settings/RestirGiSettings.h"
#include "luth/renderer/settings/SlangParitySettings.h"
#include "luth/renderer/settings/TransparencySettings.h"
#include "luth/renderer/settings/SvgfSettings.h"
#include "luth/renderer/settings/PathTraceSettings.h"
#include "luth/renderer/settings/ReflectionsSettings.h"
#include "luth/renderer/settings/EmissiveLightSettings.h"
#include "luth/renderer/settings/WindSettings.h"

#include <entt/entt.hpp>
#include <memory>
#include <string>
#include <vector>

namespace Luth
{
    class Texture;
    struct RenderSnapshot;

    // Per-frame global shader inputs (Set 0 UBO). Layout mirrors GLSL binding.
    struct GlobalUniforms {
        Mat4 viewProjection;
        Mat4 prevViewProjection;  // frame N reads frame N-1's VP (motion vectors + TAA reprojection)
        Mat4 view;
        Mat4 projection;
        Vec3 cameraPos;
        float     time;
        Mat4 lightSpaceMatrix[k_ShadowCascadeCount];
        Vec4 cascadeSplitsViewZ;
        Vec4 shadowBias;
        Vec4 shadowNormalBias;
        Vec4 cascadeTexelSize;
        float     iblIntensity;
        float     skyboxIntensity;
        float     debugVisualizeCascades;
        float     cascadeBlendWidth;
        Vec2      viewportSize;        // pixels (W, H); fragment cluster ID, screen-space reconstruction
        float     nearZ;
        float     farZ;
        // Volumetric fog params. rgb/a packing keeps the std140 ride efficient: 4 vec4 = 64 B.
        // .a of distanceFogColorDensity / heightFogColorDensity carries density (single-channel).
        // distanceFogParams: x = start, y = maxOpacity, z = enabled flag (0/1), w = pad.
        // heightFogParams:   x = refHeight, y = falloff, z = enabled flag, w = multiScatterIntensity.
        Vec4 distanceFogColorDensity;
        Vec4 distanceFogParams;
        Vec4 heightFogColorDensity;
        Vec4 heightFogParams;
        // x = anisotropy (HG g), y = temporalAlpha, z = sunFogAbsorptionSteps (cast), w = skyFogStrength.
        Vec4 volTemporalParams;
        // x = prevNearZ, y = prevFarZ, z/w = pad. Cached for cross-frame reprojection so the resolve
        // pass can reconstruct prev view-Z without assuming nearZ/farZ are constant.
        Vec4 prevViewParams;
        // x = noiseScale (world-space frequency, 1/wavelength_m), y = noiseStrength (0..1 modulation
        // amplitude), z/w pad. Drives the Worley-FBM density-noise term in the inject shader.
        Vec4 volNoiseParams;
        // xyz = wind direction x speed (m/s); animates the noise sample UV over time, w pad.
        Vec4 volNoiseWind;
        // x = scatteringIntensity (post-canonical artistic multiplier on inject_scatter's output;
        // matches UE5 "Scattering Distribution" / Frostbite multiplier). y = blueNoiseDither (1/0).
        // zw reserved.
        Vec4 volScatterParams;
        // x = specularAaEnabled (1/0), y = specularAaSigma. Tokuyoshi 2019 screen-space normal-curvature
        // variance lifted into roughness in pbr.frag; kills high-freq specular sparkle on curved metal.
        Vec4 specAaParams;
        // x = taaEnabled (1/0), y = temporalAlpha (history feedback, 0.05..0.2), zw = currentJitter (pixels).
        Vec4 taaParams;
        // xy = prevJitter (pixels, last frame's currentJitter); zw pad. Paired with taaParams.zw so
        // slim_gbuffer.slang can dejitter motion source-side (Tardif form): the producer writes pure
        // scene motion, supersedes the resolve-side push-constant jitterDelta.
        Vec4 prevJitter;
        // x = shadowingMode (0=RasterCSM, 1=RtShadows), y = rtOriginEpsilon, z = rtNormalEpsilon, w pad.
        // pbr.frag::ComputeShadow dispatches on .x; RT path reads .y/.z for ray-origin biasing (Wächter-Binder).
        Vec4 rtShadowParams;
        // x = ReSTIR DI enabled (1 when the subsystem is on AND a valid DI image exists). When set,
        // pbr.frag samples the demodulated DI image (Set 3 b5) instead of running the point-light loop.
        Vec4 restirParams;
        // Path-traced reference mode. x = enabled (RenderMode::PathTrace), y =
        // samplesPerFrame, z = maxBounces, w = accumulated sample count. PT bypasses pbr.frag entirely
        // (it overwrites the post chain's HDR input), so this is informational/debug, not a pbr.frag gate.
        Vec4 pathTraceParams;
        // RT reflections. x = enabled (1 = pbr.frag composites the denoised reflection
        // into the split-sum specular IBL), y = roughnessFadeStart, z = roughnessFadeEnd (full RT below
        // Start, smoothstep to prefiltered-env IBL, pure IBL above End), w pad.
        Vec4 reflParams;
        // depth->world for the RT-reflection denoiser's virtual reprojection. APPENDED at the end; never
        // insert mid-struct: shaders with an inline GlobalUniforms prefix (skybox.frag etc.) would desync.
        Mat4 invViewProjection;
        // Prev depth->world + prev camera pos. APPENDED; DI temporal BASIC reprojects the previous
        // surface and evaluates the winner's light there (else BASIC degenerates to biased 1/M).
        Mat4 prevInvViewProjection;
        Vec4 prevCameraPos;
    };

    // Top-level render path selector. Raster = the clustered Forward+ / ReSTIR pipeline; PathTrace = the
    // brute-force ground-truth reference. An A/B compare toggle like ShadowingMode /
    // TonemapOperator; distinct from ShadeMode (post-tonemap debug-viz blits, not a path replacement).
    enum class RenderMode : u8 { Raster = 0, PathTrace = 1 };

    enum class ShadeMode : u8 {
        Lit = 0, Unlit, Wireframe, Normals, EntityID,
        // Slim G-buffer live viz: bypasses tonemap and blits the selected attachment to LDR.
        // Implemented in PostProcessSubsystem::AddSlimVizPass via slim_viz.frag.
        SlimNormal, SlimRoughness, SlimMotion, SlimMaterialID,
        // Forward+ cluster density viz: samples SceneDepth to compute the per-fragment 3D cluster
        // ID and heat-maps the cluster's light count over LDR. LightingSubsystem::AddClusterVizPass.
        ClustersDensity,
        // Volumetric fog atlas viz: samples SceneDepth to derive the Wronski slice, then reads
        // the per-view fog atlas. Two modes: density heat-map and integrated in-scatter radiance.
        VolumetricDensity, VolumetricInScatter,
        // ReSTIR GI reservoir viz: heat-maps the spatial reservoir's M (confidence) + age (staleness).
        RestirGiReservoir,
        // Emissive radiance only: in-shader override in pbr_shade.slang; isolates emission for raster==RT A/B.
        Emission,
        // In-shader per-draw channel overrides (obj.shadeMode -> pbr_shade.slang); cheap, raster==RT-safe.
        // ShadowCascades reuses the CSM/RT cascade-tint path. APPENDED; never renumber existing values
        // (the object buffer, slim-viz offset math, and the live shader branches all key off them).
        Metallic, Occlusion, ShadowCascades,
        // Raw screen-space signals: in-shader overrides sampling the same bound textures the lit pass
        // reads (each gated on its feature; greyed in the picker when off). AO reuses gtao.visualize.
        AmbientOcclusion, GiRaw, DiRaw, RtReflectionRaw,
        // Shaded fill + a flat wireframe overlay (a second line-polygon pass in GeometrySubsystem). The fill
        // renders lit (no shader override), so it is NOT a data-debug mode: the composite tonemaps it.
        WireframeShaded
    };

    // RenderPipeline decodes Slim viz as an offset from SlimNormal; keep these four contiguous.
    static_assert(static_cast<u8>(ShadeMode::SlimMaterialID) - static_cast<u8>(ShadeMode::SlimNormal) == 3,
                  "Slim* ShadeModes must stay contiguous (RenderPipeline slim-viz offset math).");

    // Debug modes whose fragment output is display-ready DATA (encoded normals, IDs, [0,1] channels)
    // rather than radiance: the composite sRGB-encodes them WITHOUT tonemap/grade (signalled by a
    // negative tonemapOp to postprocess.slang). Radiance debug modes (Emission, GiRaw/DiRaw/ReflRaw,
    // ShadowCascades) keep tonemap; bloom is gated off for every non-Lit mode in RenderPipeline.
    inline bool IsDataDebugMode(ShadeMode m)
    {
        return m == ShadeMode::Unlit    || m == ShadeMode::Normals   || m == ShadeMode::EntityID
            || m == ShadeMode::Metallic || m == ShadeMode::Occlusion || m == ShadeMode::AmbientOcclusion;
    }

    struct GeometryOutput {
        RG::ResourceHandle color;
        RG::ResourceHandle depth;
        RG::ResourceHandle entityID;
    };

    // SlimGBufferPass outputs, written between DepthPrepass and GTAO Prefilter. Consumed by
    // TAA (motion), downstream RT denoisers (normal + roughness), RT reflections (normal).
    struct SlimGBufferOutput {
        RG::ResourceHandle normal;     // RG16F: octahedral world-space normal
        RG::ResourceHandle roughness;  // R8:    perceptual roughness
        RG::ResourceHandle motion;     // RG16F: NDC delta (currNDC - prevNDC)
        RG::ResourceHandle materialID; // R16U:  bindless material slot
    };

    struct SelectionMaskOutput {
        RG::ResourceHandle mask;
        RG::ResourceHandle depth;
    };

    // ECS-glue layer for the renderer. Owns frame-level scene inputs (CameraParams, DrawList,
    // FrameTargets) and orchestrates per-frame work by invoking RenderPipeline. Lighting inputs
    // (gatherer, cascade fit, shadow params) live on LightingSystem; RenderingSystem looks it up
    // from SystemRegistry each frame.
    //
    // Graphics resources (pipelines, descriptor sets, samplers, UBOs, SSBOs, indirect/object buffers,
    // IBL cubemaps, bloom textures, GPU timers, named-texture registry, captured graph snapshot,
    // per-draw + depth preview textures) live on RenderPipeline.
    class RenderingSystem : public ISystem
    {
    public:
        RenderingSystem(u32 viewportWidth = 1280, u32 viewportHeight = 720);
        ~RenderingSystem();

        void Update(Scene* scene) override;
        void Resize(u32 width, u32 height);

        // Queue an extra view to render this frame. Called by editor panels (e.g. GamePanel) before
        // Update; views record in queued order ahead of the scene view's subgraph. Cleared each Update.
        void QueueView(const RenderView& view) { m_QueuedViews.push_back(view); }

        // Project lifecycle hooks: extend / restrict the shader hot-reload watcher to cover the active project's shaders directory.
        void OnProjectLoaded();
        void OnProjectUnloaded();

        std::shared_ptr<Texture> GetSceneColor() const {
            const auto& ldr = m_SceneTargets.GetLDROutput();
            return ldr ? ldr : m_SceneTargets.GetSceneColor();
        }
        PostProcessSettings& GetPostProcessSettings() { return m_PostProcessSettings; }
        const PostProcessSettings& GetPostProcessSettings() const { return m_PostProcessSettings; }

        VolumetricSettings& GetVolumetricSettings() { return m_VolumetricSettings; }
        const VolumetricSettings& GetVolumetricSettings() const { return m_VolumetricSettings; }

        TransparencySettings& GetTransparencySettings() { return m_TransparencySettings; }
        const TransparencySettings& GetTransparencySettings() const { return m_TransparencySettings; }

        RestirSettings& GetRestirSettings() { return m_RestirSettings; }
        const RestirSettings& GetRestirSettings() const { return m_RestirSettings; }

        WindSettings& GetWindSettings() { return m_WindSettings; }
        const WindSettings& GetWindSettings() const { return m_WindSettings; }

        RestirGiSettings& GetRestirGiSettings() { return m_RestirGiSettings; }
        const RestirGiSettings& GetRestirGiSettings() const { return m_RestirGiSettings; }

        SlangParitySettings& GetSlangParitySettings() { return m_SlangParitySettings; }
        const SlangParitySettings& GetSlangParitySettings() const { return m_SlangParitySettings; }

        SvgfSettings& GetSvgfSettings() { return m_SvgfSettings; }
        const SvgfSettings& GetSvgfSettings() const { return m_SvgfSettings; }

        // Separate SVGF tuning for the ReSTIR GI denoiser instance: GI is a noisier, lower-frequency
        // signal already temporally accumulated by its reservoir, so it leans on wider a-trous + a
        // shorter SVGF history than DI. Surfaced as the editor's "SVGF (GI)" section.
        SvgfSettings& GetSvgfGiSettings() { return m_SvgfGiSettings; }
        const SvgfSettings& GetSvgfGiSettings() const { return m_SvgfGiSettings; }

        // Specular (RT-reflection) denoiser tuning: a sharper, view-dependent signal than diffuse GI, so
        // fewer a-trous levels (less smear on mirrors) + the hit-distance virtual reprojection carries the
        // temporal stability. Surfaced as the editor's "SVGF (Specular)" section.
        SvgfSettings& GetSvgfSpecSettings() { return m_SvgfSpecSettings; }
        const SvgfSettings& GetSvgfSpecSettings() const { return m_SvgfSpecSettings; }

        // ReSTIR-DI specular denoiser tuning; same shape as the reflection spec denoiser.
        SvgfSettings& GetSvgfDiSpecSettings() { return m_SvgfDiSpecSettings; }
        const SvgfSettings& GetSvgfDiSpecSettings() const { return m_SvgfDiSpecSettings; }

        PathTraceSettings& GetPathTraceSettings() { return m_PathTraceSettings; }
        const PathTraceSettings& GetPathTraceSettings() const { return m_PathTraceSettings; }

        // RT specular reflections. ReflectionsSubsystem::IsEnabled reads .enabled;
        // GlobalSubsystem packs the fade band into reflParams (pbr.frag composite gate). Editor "Reflections".
        ReflectionsSettings& GetReflectionsSettings() { return m_ReflectionsSettings; }
        const ReflectionsSettings& GetReflectionsSettings() const { return m_ReflectionsSettings; }

        // Emissive-as-area-lights. EmissiveLightGatherer reads .enabled / .minPowerLum; rides with ReSTIR DI.
        EmissiveLightSettings& GetEmissiveLightSettings() { return m_EmissiveLightSettings; }
        const EmissiveLightSettings& GetEmissiveLightSettings() const { return m_EmissiveLightSettings; }

        // Top-level render path (Raster / PathTrace). PathTraceSubsystem::IsEnabled() reads this; the
        // editor RenderPanel toggles it. Switching modes resets the PT accumulation on the next frame.
        RenderMode GetRenderMode() const { return m_RenderMode; }
        void SetRenderMode(RenderMode mode) { m_RenderMode = mode; }

        u64 GetFrameAllocatorUsage() const { return m_FrameAllocator->GetUsedMemory(); }
        u64 GetFrameAllocatorTotal() const { return m_FrameAllocator->GetTotalSize(); }

        const RG::RenderGraphSnapshot& GetGraphSnapshot() const;
        std::shared_ptr<Texture> GetNamedTexture(const std::string& name) const;

        u32 GetTriangleCount() const { return m_DrawList.visibleTriCount; }

        ShadeMode GetShadeMode() const { return m_ShadeMode; }
        void SetShadeMode(ShadeMode mode) { m_ShadeMode = mode; }

        // Accessors used by PickingSystem (readback reads the EntityID target and maps the sampled index
        // back to an entity via the Pipeline) and GamePanel (owns its own FrameTargets, shares the Pipeline).
        FrameTargets&   GetSceneTargets() { return m_SceneTargets; }
        RenderPipeline& GetPipeline()     { return *m_Pipeline; }

        // Renderer-side accessors. The render path reads these per frame (passes consume DrawList +
        // CameraParams; debugger captures via FrameDebugger; RG allocators come from FrameAllocator).
        FrameDebugger&             GetFrameDebugger()       { return m_FrameDebugger; }
        const FrameDebugger&       GetFrameDebugger() const { return m_FrameDebugger; }
        Memory::LinearAllocator&   GetFrameAllocator()      { return *m_FrameAllocator; }
        const DrawList&            GetDrawList() const      { return m_DrawList; }
        const CameraParams&        GetCameraParams() const  { return m_CameraParams; }

        // Selection outline + editor grid params flow through CameraParams (populated in App.cpp from EditorViewportState each frame).

        // Skybox / IBL
        void ReloadSkybox(const std::filesystem::path& hdrPath);

        // Editor grid visibility (screen-space infinite grid pass)
        void SetGridVisible(bool v) { m_GridVisible = v; }
        bool IsGridVisible() const  { return m_GridVisible; }

        // Camera / editor state (set by App each frame before Update)
        void SetCameraParams(const CameraParams& params) { m_CameraParams = params; }

        // Snapshot the renderer reads this frame. Set by Update before passes record; consumed by passes
        // (Frame Debugger tag lookups, etc.) via the RenderPipeline friend access. Lifetime is one frame.
        const RenderSnapshot& GetActiveSnapshot() const { return *m_ActiveSnapshot; }

        // Frame debugger capture
        void RequestCapture()   { if (m_FrameDebugger.state == DebuggerState::Inactive) m_FrameDebugger.state = DebuggerState::CaptureRequested; }
        void ExitCapture();
        DebuggerState GetDebuggerState() const { return m_FrameDebugger.state; }
        const RG::CapturedFrame& GetCapturedFrame() const { return m_FrameDebugger.capturedFrame; }
        VkSampler GetDebugSampler() const { return m_FrameDebugger.sampler; }

        // Capture source: Scene (editor camera) or Game (hierarchy camera). Mutating only changes the
        // *next* capture; Frozen overlays use capturedSource so a mid-capture toggle doesn't redirect
        // the overlay to a viewport whose camera never produced these archives.
        void          SetCaptureSource(CaptureSource s) { m_FrameDebugger.requestedSource = s; }
        CaptureSource GetCaptureSource() const          { return m_FrameDebugger.requestedSource; }
        CaptureSource GetCapturedSource() const         { return m_FrameDebugger.capturedSource; }

        // Frame-debugger preview forwarders (implementations on RenderPipeline).
        void        ReplayPassUpToDraw(u32 passIdx, u32 localDrawIdx);
        VkImageView GetPerDrawPreviewView() const;
        u64         GetPerDrawPreviewKey()  const;
        u32         GetPerDrawPreviewWidth()  const;
        u32         GetPerDrawPreviewHeight() const;

        void        BlitArchivedDepthToPreview(u32 archiveIdx, int layer, float nearZ, float farZ);
        VkImageView GetDepthPreviewView()   const;
        u32         GetDepthPreviewWidth()  const;
        u32         GetDepthPreviewHeight() const;

        void        BlitArchivedSlimToPreview(u32 archiveIdx, u32 mode, float scale);
        VkImageView GetSlimPreviewView()    const;
        u32         GetSlimPreviewWidth()   const;
        u32         GetSlimPreviewHeight()  const;

    private:
        // Run the per-view prep chain (lighting fit, PrepareForTargets, UBO uploads) and record the subgraph
        // into the view's QueueRecorders triplet. Returns true iff the graph routed any pass to async-compute;
        // forwarded to Renderer::EndPrimaryCmdAndSubmit so SubmitView knows whether to issue the compute submit.
        // Cross-view RAW sync for shared resources (m_ShadowMap) is enforced by the per-view 3-submit topology's
        // timeline waits at submit boundaries. See arch/multi-queue.md.
        bool RecordView(const RenderView& view, QueueRecorders recorders);

        // Camera / editor state set each frame by App.
        CameraParams m_CameraParams;

        // Extra views queued by editor panels; drained each Update.
        std::vector<RenderView> m_QueuedViews;

        // Memory.
        std::unique_ptr<Memory::LinearAllocator> m_FrameAllocator;

        // Scene panel's render targets. GamePanel owns its own FrameTargets so the two views resize independently.
        FrameTargets m_SceneTargets;

        // Per-frame draw list (RenderMode-sorted buckets + tri count).
        DrawListBuilder m_DrawListBuilder;
        DrawList        m_DrawList;

        // Graphics resources + render-graph orchestration (owns all pipelines,
        // descriptor sets, samplers, UBOs, SSBOs, preview textures, etc.).
        std::unique_ptr<RenderPipeline> m_Pipeline;

        // Editor-facing state.
        PostProcessSettings  m_PostProcessSettings;
        VolumetricSettings   m_VolumetricSettings;
        TransparencySettings m_TransparencySettings;
        RestirSettings       m_RestirSettings;
        RestirGiSettings     m_RestirGiSettings;
        SlangParitySettings   m_SlangParitySettings;
        SvgfSettings         m_SvgfSettings;
        // GI denoiser defaults: lower history cap + shorter temporal alpha + one more a-trous level.
        SvgfSettings         m_SvgfGiSettings{ .alphaColor = 0.3f, .alphaMoments = 0.3f, .historyCap = 16u, .atrousIterations = 6u };
        // Specular denoiser: fewer a-trous levels (preserve mirror sharpness), moderate temporal alpha.
        SvgfSettings         m_SvgfSpecSettings{ .alphaColor = 0.15f, .alphaMoments = 0.15f, .historyCap = 24u, .atrousIterations = 3u };
        SvgfSettings         m_SvgfDiSpecSettings{ .alphaColor = 0.15f, .alphaMoments = 0.15f, .historyCap = 24u, .atrousIterations = 3u };  // ReSTIR-DI specular
        PathTraceSettings    m_PathTraceSettings;
        ReflectionsSettings  m_ReflectionsSettings;
        EmissiveLightSettings m_EmissiveLightSettings;
        WindSettings         m_WindSettings;
        RenderMode           m_RenderMode   = RenderMode::Raster;
        ShadeMode            m_ShadeMode    = ShadeMode::Lit;
        bool                m_GridVisible  = true;

        // Frame debugger runtime state (capture state machine + archives).
        FrameDebugger m_FrameDebugger;

        // Snapshot consumed by passes this frame (set in Update; non-owning).
        const RenderSnapshot* m_ActiveSnapshot = nullptr;
    };
}
