#pragma once

#include "luth/core/LuthTypes.h"

#include <memory>

namespace Luth
{
    class Texture;

    // Persistent viewport-sized render targets shared across passes.
    // Owns SceneColor / SceneDepth / EntityID / LDR / Selection {mask,depth}.
    // Allocate() runs once after the backend exists; Resize() recreates them
    // when the viewport changes.
    class FrameTargets
    {
    public:
        FrameTargets() = default;

        void Allocate(u32 width, u32 height);
        void Resize(u32 width, u32 height);

        const std::shared_ptr<Texture>& GetSceneColor()     const { return m_SceneColor; }
        const std::shared_ptr<Texture>& GetSceneDepth()     const { return m_SceneDepth; }
        const std::shared_ptr<Texture>& GetEntityIDBuffer() const { return m_EntityIDBuffer; }
        const std::shared_ptr<Texture>& GetLDROutput()      const { return m_LDROutput; }
        const std::shared_ptr<Texture>& GetSelectionMask()  const { return m_SelectionMask; }
        const std::shared_ptr<Texture>& GetSelectionDepth() const { return m_SelectionDepth; }

        bool IsAllocated() const { return m_SceneColor != nullptr; }

    private:
        std::shared_ptr<Texture> m_SceneColor;
        std::shared_ptr<Texture> m_SceneDepth;
        std::shared_ptr<Texture> m_EntityIDBuffer;
        std::shared_ptr<Texture> m_LDROutput;
        std::shared_ptr<Texture> m_SelectionMask;
        std::shared_ptr<Texture> m_SelectionDepth;
    };
}
