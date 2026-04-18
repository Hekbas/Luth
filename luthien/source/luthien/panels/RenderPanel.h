#pragma once

#include "luthien/Editor.h"
#include "luth/core/UUID.h"
#include "luth/scene/systems/RenderingSystem.h"

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
        
        RenderingSystem* m_RS = nullptr;
        std::string m_SelectedMode;
        u32 m_SelectedAttachment = 0;

        u32 m_SelectedTab = 0; // 0 for Model Viewer, 1 for Post Processing
    };
}
