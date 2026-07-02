#pragma once

#include "luth/core/types/LuthTypes.h"
#include "luth/renderer/rendergraph/RenderGraph.h"

#include <string>
#include <vector>

namespace Luth
{
    class FrameTargets;
    class RenderPipeline;
    struct ViewResources;

    // Inputs a denoiser consumes. The diffuse SVGF path uses di/depth/normal/motion/materialID;
    // roughness/hitDist/confidence are reserved (zero-fed for now) so an NRD/RELAX swap and a future
    // specular channel satisfy the same contract without an interface break. di carries DEMODULATED
    // diffuse irradiance E = Li*NdotL*W; materials are reapplied after denoising. see arch/rendering-pipeline.md
    struct DenoiseInputs
    {
        RG::ResourceHandle di;          // noisy demodulated diffuse irradiance (the signal to denoise)
        RG::ResourceHandle depth;       // scene depth (linearized in-shader for edge stops)
        RG::ResourceHandle normal;      // slim G-buffer octahedral world normal (RG16F)
        RG::ResourceHandle motion;      // slim G-buffer de-jittered NDC motion (RG16F)
        RG::ResourceHandle roughness;   // slim G-buffer perceptual roughness (R8); reserved
        RG::ResourceHandle materialID;  // slim G-buffer bindless material slot (R16U); disocclusion ID
        RG::ResourceHandle hitDist;     // reserved: NRD radiance+hitDist packing + future specular
        RG::ResourceHandle confidence;  // reserved: ReSTIR M-count history-confidence prior
    };

    // Swappable real-time denoiser behind a settings toggle (custom SVGF today; NRD/RELAX later). The
    // interface mirrors the render-subsystem lifecycle so the pipeline wires it like any other domain.
    // AddPasses consumes the demodulated DI and returns the denoised handle the GeometryPass reads and
    // the lighting set binds; an invalid input handle returns invalid (the consumer then skips the read
    // and pbr.frag runs its own light loop). Per-view history/output images live on ViewResources, so
    // the implementation owns its descriptor-set allocation + binding against the shared view pool.
    class IDenoiser
    {
    public:
        virtual ~IDenoiser() = default;

        virtual void Init(RenderPipeline& pipeline) = 0;
        virtual void Shutdown() = 0;
        virtual bool OnShaderReloaded(const std::string& name, const std::vector<u32>& spv) = 0;

        // Allocate the denoiser's per-view descriptor sets from vr.descPool (called when a view is
        // first created). WriteView (re)points them at the view's images (also called on resize).
        virtual void AllocateViewSets(ViewResources& vr) = 0;
        virtual void WriteView(ViewResources& vr, FrameTargets& targets) = 0;

        virtual RG::ResourceHandle AddPasses(RG::RenderGraph& rg, const DenoiseInputs& in) = 0;

        virtual bool IsEnabled() const = 0;
    };
}
