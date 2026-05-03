#pragma once

#include "luth/core/UUID.h"
#include "luth/resources/Image.h"

#include <memory>

namespace Luth
{
    class Material;
    class Model;
}

namespace Luth::UI
{
    // Synchronous offscreen mesh-thumbnail bake. Composes with the engine's
    // VKPipeline + VKTexture + VulkanContext::ImmediateSubmit; one bake per
    // call, ~1 ms typical. Caller must be on the main thread (Vulkan submit
    // affinity); ThumbnailGenerator hops worker → main via MainThreadPump.
    //
    // invariant: persistent resources (pipeline, color/depth targets, staging
    // buffer) are created in Init and reused across bakes — no per-bake VMA
    // allocations. Lifetime tied to Editor::Init / Editor::Shutdown.
    namespace ThumbnailPreviewScene
    {
        // Public lifecycle, called from Editor::Init / Editor::Shutdown.
        // Idempotent. Init returns false on shader / pipeline failure.
        bool Init();
        void Shutdown();

        // Bakes every sub-mesh of `model` (non-skinned and skinned both
        // supported via two pipeline variants) into a 128×128 RGBA8 image with
        // a neutral white albedo. Returns invalid LoadResult8 on any failure.
        Image::LoadResult8 BakeMesh(const std::shared_ptr<Model>& model);

        // Bakes the engine's Sphere primitive with the material's albedo
        // color, producing a uniform "this is the material's tint" preview.
        // Returns invalid LoadResult8 if the sphere primitive is unavailable.
        Image::LoadResult8 BakeMaterial(const std::shared_ptr<Material>& material);

        // Output thumbnail size — square. Matches BakeTexture's targetSize so
        // disk persistence and ImGui display dimensions stay uniform.
        u32 GetSize();
    }
}
