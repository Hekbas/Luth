#pragma once

#include "luth/editor/Editor.h"
#include "luth/core/UUID.h"
#include "luth/ECS/systems/RenderingSystem.h"

#include <map>
#include <string>
#include <functional>

namespace Luth
{
    class RenderPanel : public Panel
    {
    public:
        RenderPanel();

        void OnInit() override;
        void OnRender() override;

        u32 GetSelectedAttachment() { return m_SelectedAttachment; }

        // Shader management
        void SetShaderOverride(bool override) { m_IsShaderOverride = override; }
        bool IsShaderOverriden() const { return m_IsShaderOverride; }
        UUID GetSelectedShader() const { return m_ShaderOverride; }

        // Rendering parameters
        void SetWireframe(bool wireframe) { m_Wireframe = wireframe; }
        bool IsWireframe() const { return m_Wireframe; }

    private:
        void ApplyRenderingSettings();

        // Rendering system reference
        std::weak_ptr<RenderingSystem> m_RenderingSystem;

        // Selected Framebuffer attachment
        i32 m_SelectedAttachment = -1;

        // Shader controls
        UUID m_ShaderOverride;
        bool m_IsShaderOverride = false;

        // Rendering state
        bool m_Wireframe = false;
        bool m_BackfaceCulling = true;
        bool m_DepthTest = true;
        ImVec4 m_ClearColor = { 0.45f, 0.55f, 0.60f, 1.00f };
    };
}
