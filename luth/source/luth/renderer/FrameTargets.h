#pragma once

#include "luth/core/types/LuthTypes.h"

#include <memory>

namespace Luth
{
    class Texture;

    // Persistent viewport-sized render targets shared across passes.
    // Owns SceneColor / SceneDepth / EntityID / LDR / Selection {mask,depth} and the 4 slim
    // G-buffer attachments (normal / roughness / motion vectors / material ID).
    // Allocate() runs once after the backend exists; Resize() recreates them when the viewport changes.
    class FrameTargets
    {
    public:
        FrameTargets() = default;

        void Allocate(u32 width, u32 height);
        void Resize(u32 width, u32 height);

        const std::shared_ptr<Texture>& GetSceneColor()       const { return m_SceneColor; }
        const std::shared_ptr<Texture>& GetSceneDepth()       const { return m_SceneDepth; }
        const std::shared_ptr<Texture>& GetEntityIDBuffer()   const { return m_EntityIDBuffer; }
        const std::shared_ptr<Texture>& GetLDROutput()        const { return m_LDROutput; }
        const std::shared_ptr<Texture>& GetSelectionMask()    const { return m_SelectionMask; }
        const std::shared_ptr<Texture>& GetSelectionDepth()   const { return m_SelectionDepth; }
        const std::shared_ptr<Texture>& GetSlimNormal()       const { return m_SlimNormal; }
        const std::shared_ptr<Texture>& GetSlimRoughness()    const { return m_SlimRoughness; }
        const std::shared_ptr<Texture>& GetSlimMotion()       const { return m_SlimMotion; }
        const std::shared_ptr<Texture>& GetSlimMaterialID()   const { return m_SlimMaterialID; }

        bool IsAllocated() const { return m_SceneColor != nullptr; }

    private:
        std::shared_ptr<Texture> m_SceneColor;
        std::shared_ptr<Texture> m_SceneDepth;
        std::shared_ptr<Texture> m_EntityIDBuffer;
        std::shared_ptr<Texture> m_LDROutput;
        std::shared_ptr<Texture> m_SelectionMask;
        std::shared_ptr<Texture> m_SelectionDepth;
        // Slim G-buffer (Phase A.2). Written by SlimGBufferPass between DepthPrepass and GTAO.
        std::shared_ptr<Texture> m_SlimNormal;     // RG16F — octahedral world-space normal
        std::shared_ptr<Texture> m_SlimRoughness;  // R8    — perceptual roughness
        std::shared_ptr<Texture> m_SlimMotion;     // RG16F — NDC motion delta (currNDC - prevNDC)
        std::shared_ptr<Texture> m_SlimMaterialID; // R16U  — bindless material slot
    };
}
