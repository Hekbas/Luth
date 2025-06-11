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

        u32 GetSelectedAttachment() const { return m_SelectedAttachment; }

    private:
        // group definitions
        const std::vector<std::pair<const char*, std::vector<const char*>>> m_Groups = {
          { "RENDER",
            { "Final", "No Post-Processing" }},
          { "ANIMATION",
            { "Bones", "Bones Influence" } },
          { "MATERIAL CHANNELS",
            { "Base Color", "Metalness", "Roughness", "Normal Map", "Emission", "Specular F0", "Translucency", "Ambient Oclusion", "Opacity" }},
          { "GEOMETRY",
            { "Position", "Normal", "MRAO", "ET", "SSAO", "Wireframe" } }
        };

        void DrawGroup(const char* title, const std::vector<const char*>& modes);
        void ApplyRenderingSettings();
        
        std::shared_ptr<RenderingSystem> m_RS;
        std::string m_SelectedMode;
        u32 m_SelectedAttachment = 0;

        u32 m_SelectedTab = 0; // 0 for Model Viewer, 1 for Post Processing
    };
}
