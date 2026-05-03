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

    EditState Property(const char* label, std::string& value)
    {
        PropertyLabel(label);

        char buffer[256];
        memset(buffer, 0, sizeof(buffer));
        strncpy_s(buffer, value.c_str(), sizeof(buffer));

        std::string id = "##" + std::string(label);
        std::string pre = value;

        bool changed = ImGui::InputText(id.c_str(), buffer, sizeof(buffer));
        ImGuiID itemId = ImGui::GetItemID();
        if (changed) value = std::string(buffer);

        EditState st{ changed, false, itemId };
        if (ImGui::IsItemActivated())            SaveItemPreEdit<std::string>(itemId, pre);
        if (ImGui::IsItemDeactivatedAfterEdit()) st.committed = true;

        ImGui::PopItemWidth();
        return st;
    }

    EditState Property(const char* label, bool& value)
    {
        PropertyLabel(label);
        std::string id = "##" + std::string(label);
        bool pre = value;

        bool changed = ImGui::Checkbox(id.c_str(), &value);
        ImGuiID itemId = ImGui::GetItemID();

        EditState st{ changed, false, itemId };
        // Checkbox is discrete (one toggle per click). Synchronous commit:
        // IsItemDeactivatedAfterEdit is unreliable for click-toggle widgets where
        // activation + edit + deactivation can collapse into a single frame.
        if (changed) {
            SaveItemPreEdit<bool>(itemId, pre);
            st.committed = true;
        }

        ImGui::PopItemWidth();
        return st;
    }

    EditState Property(const char* label, int& value, int min, int max)
    {
        PropertyLabel(label);
        std::string id = "##" + std::string(label);
        int pre = value;

        bool changed;
        if (min == 0 && max == 0)
            changed = ImGui::DragInt(id.c_str(), &value);
        else
            changed = ImGui::DragInt(id.c_str(), &value, 1.0f, min, max);
        ImGuiID itemId = ImGui::GetItemID();

        EditState st{ changed, false, itemId };
        if (ImGui::IsItemActivated())            SaveItemPreEdit<int>(itemId, pre);
        if (ImGui::IsItemDeactivatedAfterEdit()) st.committed = true;

        ImGui::PopItemWidth();
        return st;
    }

    EditState Property(const char* label, float& value, float speed, float min, float max)
    {
        PropertyLabel(label);
        std::string id = "##" + std::string(label);
        float pre = value;

        bool changed = ImGui::DragFloat(id.c_str(), &value, speed, min, max, "%.3f");
        ImGuiID itemId = ImGui::GetItemID();

        EditState st{ changed, false, itemId };
        if (ImGui::IsItemActivated())            SaveItemPreEdit<float>(itemId, pre);
        if (ImGui::IsItemDeactivatedAfterEdit()) st.committed = true;

        ImGui::PopItemWidth();
        return st;
    }

    // Multi-axis vector control. Pre-edit snapshot is the WHOLE composite, keyed by row label,
    // saved once on the first axis to activate this frame. Commit fires when ANY axis is
    // deactivated-after-edit. Reset button is treated as a synchronous one-frame commit.
    template <typename T>
    static EditState DrawVecControlT(const char* label, T& value, float resetValue, float speed)
    {
        constexpr int components = sizeof(T) / sizeof(float);
        static_assert(components >= 2 && components <= 4, "Vec must be 2/3/4 floats");
        float* values = &value.x;

        PropertyLabel(label);
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2{ 0, 0 });

        float lineHeight = GImGui->Font->FontSize + GImGui->Style.FramePadding.y * 2.0f;
        ImVec2 buttonSize = { lineHeight + 3.0f, lineHeight };

        float totalButtonWidth = components * buttonSize.x;
        ImGui::PushMultiItemsWidths(components, ImGui::CalcItemWidth() - totalButtonWidth);

        EditState st;
        T preComposite = value;            // captured at entry; on activation frame this == pre-drag state
        ImGuiID rowId = ImGui::GetID(label);
        st.itemId = rowId;
        bool savedThisFrame = false;

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
                st.changed = true;
                // Buttons don't emit IsItemDeactivatedAfterEdit. Commit synchronously.
                if (!savedThisFrame) { SaveItemPreEdit<T>(rowId, preComposite); savedThisFrame = true; }
                st.committed = true;
            }
            ImGui::PopStyleColor(3);

            ImGui::SameLine();
            std::string dragId = "##" + std::string(label) + std::to_string(i);
            if (ImGui::DragFloat(dragId.c_str(), &values[i], speed, 0.0f, 0.0f, "%.2f"))
                st.changed = true;
            if (ImGui::IsItemActivated() && !savedThisFrame) {
                SaveItemPreEdit<T>(rowId, preComposite);
                savedThisFrame = true;
            }
            if (ImGui::IsItemDeactivatedAfterEdit()) st.committed = true;

            if (i < components - 1)
                ImGui::SameLine();

            ImGui::PopItemWidth();
        }

        ImGui::PopStyleVar();
        ImGui::PopItemWidth(); // Pop the one pushed in PropertyLabel
        return st;
    }

    EditState Property(const char* label, Vec2& value, float speed, float resetValue)
    {
        return DrawVecControlT<Vec2>(label, value, resetValue, speed);
    }

    EditState Property(const char* label, Vec3& value, float speed, float resetValue)
    {
        return DrawVecControlT<Vec3>(label, value, resetValue, speed);
    }

    EditState Property(const char* label, Vec4& value, float speed, float resetValue)
    {
        return DrawVecControlT<Vec4>(label, value, resetValue, speed);
    }

    EditState PropertyColor(const char* label, Vec3& value)
    {
        PropertyLabel(label);
        std::string id = "##" + std::string(label);
        Vec3 pre = value;

        bool changed = ImGui::ColorEdit3(id.c_str(), &value.x);
        ImGuiID itemId = ImGui::GetItemID();

        EditState st{ changed, false, itemId };
        if (ImGui::IsItemActivated())            SaveItemPreEdit<Vec3>(itemId, pre);
        if (ImGui::IsItemDeactivatedAfterEdit()) st.committed = true;

        ImGui::PopItemWidth();
        return st;
    }

    EditState PropertyColor(const char* label, Vec4& value)
    {
        PropertyLabel(label);
        std::string id = "##" + std::string(label);
        Vec4 pre = value;

        bool changed = ImGui::ColorEdit4(id.c_str(), &value.x);
        ImGuiID itemId = ImGui::GetItemID();

        EditState st{ changed, false, itemId };
        if (ImGui::IsItemActivated())            SaveItemPreEdit<Vec4>(itemId, pre);
        if (ImGui::IsItemDeactivatedAfterEdit()) st.committed = true;

        ImGui::PopItemWidth();
        return st;
    }

    EditState PropertyCombo(const char* label, int& currentIndex, const char* const items[], int count)
    {
        PropertyLabel(label);
        std::string id = "##" + std::string(label);
        int pre = currentIndex;

        bool changed = ImGui::Combo(id.c_str(), &currentIndex, items, count);
        ImGuiID itemId = ImGui::GetItemID();

        EditState st{ changed, false, itemId };
        // Combo is discrete: only true changes flip `changed`. Synchronous commit.
        // Avoids the IsItemDeactivatedAfterEdit false-positive ImGui flags for Combo.
        if (changed) {
            SaveItemPreEdit<int>(itemId, pre);
            st.committed = true;
        }

        ImGui::PopItemWidth();
        return st;
    }
}
