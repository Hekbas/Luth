#pragma once

#include "luth/core/UUID.h"
#include "luth/resources/Image.h"

#include <memory>

namespace Luth
{
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

        // Bakes the model's first non-skinned mesh into a 128×128 RGBA8 image
        // and returns the CPU pixels. Returns invalid LoadResult8 on any
        // failure (uninitialized, model missing first mesh, skinned mesh,
        // empty AABB, etc.).
        Image::LoadResult8 BakeMesh(const std::shared_ptr<Model>& model);

        // Output thumbnail size — square. Matches BakeTexture's targetSize so
        // disk persistence and ImGui display dimensions stay uniform.
        u32 GetSize();
    }
}
