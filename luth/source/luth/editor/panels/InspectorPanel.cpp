#include "luthpch.h"
#include "luth/editor/panels/InspectorPanel.h"
#include "luth/scene/Components.h"


namespace Luth
{
    void InspectorPanel::OnInit() {}

    void InspectorPanel::OnRender()
    {
        ImGui::Begin("Inspector");
        ImGui::Text("Such empty, very wow.");
        ImGui::End();
    }
}
