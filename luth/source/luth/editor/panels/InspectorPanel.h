#pragma once

#include "luth/editor/Editor.h"

namespace Luth
{
    class InspectorPanel : public Panel
    {
    public:
        void OnRender() override
        {
            ImGui::Begin("Inspector");
            // Add stuff here
            ImGui::Text("Such empty, very wow.");
            ImGui::End();
        }
    };
}
