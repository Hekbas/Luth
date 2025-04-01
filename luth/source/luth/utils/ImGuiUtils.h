#pragma once

#include "luth/core/Log.h"

#include <functional>
#include <glm/glm.hpp>
#include <imgui.h>

namespace Luth
{
    // Conversion from ImVec2 to glm::vec2
    inline glm::vec2 ToGlmVec2(const ImVec2& vec) {
        return { vec.x, vec.y };
    }

    // Conversion from glm::vec2 to ImVec2  
    inline ImVec2 ToImVec2(const glm::vec2& vec) {
        return { vec.x, vec.y };
    }

    inline glm::vec2 operator*(const glm::vec2& lhs, const ImVec2& rhs) {
        return { lhs.x * rhs.x, lhs.y * rhs.y };
    }

    // Center-aligns the next widget in the remaining horizontal space
    inline void AlignItemToCenter(float itemWidth = 0.0f)
    {
        ImGui::SetNextItemWidth(itemWidth);
        ImGuiStyle& style = ImGui::GetStyle();
        float availableWidth = ImGui::GetContentRegionAvail().x - 2.0f * style.FramePadding.x;
        float offset = (availableWidth - (itemWidth > 0.0f ? itemWidth : ImGui::CalcItemWidth())) * 0.5f;
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + std::max(0.0f, offset));
    }

    // Center-align specific text
    inline void AlignTextToCenter(const char* text)
    {
        float textWidth = ImGui::CalcTextSize(text).x;
        float availableWidth = ImGui::GetContentRegionAvail().x;
        ImGui::SetCursorPosX((availableWidth - textWidth) * 0.5f);
        ImGui::TextUnformatted(text);
    }

    inline bool ButtonDropdown(const char* label, const char* str_id, std::function<void()> menuContents)
    {
        LH_CORE_ASSERT(str_id != nullptr, "Dropdown button requires an explicit ID!");

        // Generate unique ID based on label
        ImGuiID id = ImGui::GetID(str_id);
        static std::unordered_map<ImGuiID, bool> openStates;
        static std::unordered_map<ImGuiID, ImVec2> buttonPositions;

        bool pressed = false;

        // Button
        if (ImGui::Button(label))
        {
            buttonPositions[id] = ImGui::GetItemRectMin();
            buttonPositions[id].y += ImGui::GetFrameHeight();
            openStates[id] = true;
        }

        // Handle popup opening
        if (openStates[id])
        {
            ImGui::SetNextWindowPos(buttonPositions[id]);
            ImGui::OpenPopup("##PopupButtonMenu");
            openStates[id] = false;
        }

        // Draw popup
        bool menuActive = false;
        if (ImGui::BeginPopup("##PopupButtonMenu", ImGuiWindowFlags_NoMove))
        {
            menuActive = true;
            menuContents();
            ImGui::EndPopup();
        }

        // Return true only if the popup is open now
        return menuActive;
    }
}
