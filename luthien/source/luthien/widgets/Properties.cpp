#include "lepch.h"
#include "luthien/widgets/Properties.h"
#include "luthien/EditorColors.h"

#include <imgui.h>
#include <imgui/imgui_internal.h>


namespace Luth::UI
{
    bool BeginProperties(const char* id)
    {
        ImGui::PushStyleVar(ImGuiStyleVar_CellPadding, ImVec2(4, 4));
        bool open = ImGui::BeginTable(id ? id : "Properties", 2, ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_Resizable);
        if (open)
        {
            ImGui::TableSetupColumn("Label", ImGuiTableColumnFlags_None, 1.0f);
            ImGui::TableSetupColumn("Value", ImGuiTableColumnFlags_None, 2.0f);
        }
        else
        {
            ImGui::PopStyleVar();
        }
        return open;
    }

    void EndProperties()
    {
        ImGui::EndTable();
        ImGui::PopStyleVar();
    }

    void PropertyLabel(const char* label)
    {
        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted(label);
        ImGui::TableNextColumn();
        ImGui::PushItemWidth(-1);
    }

    bool Property(const char* label, std::string& value)
    {
        PropertyLabel(label);

        char buffer[256];
        memset(buffer, 0, sizeof(buffer));
#ifdef _WIN32
        strncpy_s(buffer, value.c_str(), sizeof(buffer));
#else
        strncpy(buffer, value.c_str(), sizeof(buffer));
        buffer[sizeof(buffer) - 1] = '\0';
#endif

        std::string id = "##" + std::string(label);
        if (ImGui::InputText(id.c_str(), buffer, sizeof(buffer)))
        {
            value = std::string(buffer);
            ImGui::PopItemWidth();
            return true;
        }
        ImGui::PopItemWidth();
        return false;
    }

    bool Property(const char* label, bool& value)
    {
        PropertyLabel(label);
        std::string id = "##" + std::string(label);
        bool changed = ImGui::Checkbox(id.c_str(), &value);
        ImGui::PopItemWidth();
        return changed;
    }

    bool Property(const char* label, int& value, int min, int max)
    {
        PropertyLabel(label);
        std::string id = "##" + std::string(label);

        bool changed;
        if (min == 0 && max == 0)
            changed = ImGui::DragInt(id.c_str(), &value);
        else
            changed = ImGui::DragInt(id.c_str(), &value, 1.0f, min, max);

        ImGui::PopItemWidth();
        return changed;
    }

    bool Property(const char* label, float& value, float speed, float min, float max)
    {
        PropertyLabel(label);
        std::string id = "##" + std::string(label);
        bool changed = ImGui::DragFloat(id.c_str(), &value, speed, min, max, "%.3f");
        ImGui::PopItemWidth();
        return changed;
    }

    static bool DrawVecControl(const char* label, float* values, int components, float resetValue, float speed)
    {
        PropertyLabel(label);

        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2{ 0, 0 });

        float lineHeight = GImGui->Font->FontSize + GImGui->Style.FramePadding.y * 2.0f;
        ImVec2 buttonSize = { lineHeight + 3.0f, lineHeight };

        // Subtract button widths from available space so DragFloats don't overflow
        float totalButtonWidth = components * buttonSize.x;
        ImGui::PushMultiItemsWidths(components, ImGui::CalcItemWidth() - totalButtonWidth);

        bool changed = false;
        const char* axisLabels[] = { "X", "Y", "Z", "W" };
        const ImVec4 axisColors[] = {
            EditorColors::AxisX,
            EditorColors::AxisY,
            EditorColors::AxisZ,
            EditorColors::AxisW
        };

        for (int i = 0; i < components; i++)
        {
            ImGui::PushStyleColor(ImGuiCol_Button, axisColors[i]);
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, { axisColors[i].x + 0.1f, axisColors[i].y + 0.1f, axisColors[i].z + 0.1f, 1.0f });
            ImGui::PushStyleColor(ImGuiCol_ButtonActive, { axisColors[i].x + 0.2f, axisColors[i].y + 0.2f, axisColors[i].z + 0.2f, 1.0f });

            std::string buttonId = std::string(axisLabels[i]) + "##" + label + std::to_string(i);
            if (ImGui::Button(buttonId.c_str(), buttonSize))
            {
                values[i] = resetValue;
                changed = true;
            }
            ImGui::PopStyleColor(3);

            ImGui::SameLine();
            std::string dragId = "##" + std::string(label) + std::to_string(i);
            if (ImGui::DragFloat(dragId.c_str(), &values[i], speed, 0.0f, 0.0f, "%.2f"))
                changed = true;

            if (i < components - 1)
                ImGui::SameLine();

            ImGui::PopItemWidth();
        }

        ImGui::PopStyleVar();
        ImGui::PopItemWidth(); // Pop the one pushed in PropertyLabel
        return changed;
    }

    bool Property(const char* label, Vec2& value, float speed, float resetValue)
    {
        return DrawVecControl(label, &value.x, 2, resetValue, speed);
    }

    bool Property(const char* label, Vec3& value, float speed, float resetValue)
    {
        return DrawVecControl(label, &value.x, 3, resetValue, speed);
    }

    bool Property(const char* label, Vec4& value, float speed, float resetValue)
    {
        return DrawVecControl(label, &value.x, 4, resetValue, speed);
    }

    bool PropertyColor(const char* label, Vec3& value)
    {
        PropertyLabel(label);
        std::string id = "##" + std::string(label);
        bool changed = ImGui::ColorEdit3(id.c_str(), &value.x);
        ImGui::PopItemWidth();
        return changed;
    }

    bool PropertyColor(const char* label, Vec4& value)
    {
        PropertyLabel(label);
        std::string id = "##" + std::string(label);
        bool changed = ImGui::ColorEdit4(id.c_str(), &value.x);
        ImGui::PopItemWidth();
        return changed;
    }

    bool PropertyCombo(const char* label, int& currentIndex, const char* const items[], int count)
    {
        PropertyLabel(label);
        std::string id = "##" + std::string(label);
        bool changed = ImGui::Combo(id.c_str(), &currentIndex, items, count);
        ImGui::PopItemWidth();
        return changed;
    }
}
