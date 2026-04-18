#pragma once

#include "luth/core/Log.h"

#include <functional>
#include <imgui.h>

namespace Luth
{
    // Conversion from ImVec2 to Vec2
    inline Vec2 ToGlmVec2(const ImVec2& vec) {
        return { vec.x, vec.y };
    }

    // Conversion from Vec2 to ImVec2  
    inline ImVec2 ToImVec2(const Vec2& vec) {
        return { vec.x, vec.y };
    }

    inline Vec2 operator*(const Vec2& lhs, const ImVec2& rhs) {
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

        ImGui::PushID(str_id);

        // Button
        if (ImGui::Button(label))
        {
            ImVec2 popupPos = ImGui::GetItemRectMin();
            popupPos.y += ImGui::GetFrameHeight();
            ImGui::SetNextWindowPos(popupPos);
            ImGui::OpenPopup("##PopupButtonMenu");
        }

        // Draw popup
        bool menuActive = false;
        if (ImGui::BeginPopup("##PopupButtonMenu", ImGuiWindowFlags_NoMove))
        {
            menuActive = true;
            menuContents();
            ImGui::EndPopup();
        }

        ImGui::PopID();

        // Return true only if the popup is open now
        return menuActive;
    }

    static inline ImVec2 operator*(const ImVec2& lhs, const float rhs) {
        return ImVec2(lhs.x * rhs, lhs.y * rhs);
    }
    static inline ImVec2 operator/(const ImVec2& lhs, const float rhs) {
        return ImVec2(lhs.x / rhs, lhs.y / rhs);
    }
    static inline ImVec2 operator+(const ImVec2& lhs, const ImVec2& rhs) {
        return ImVec2(lhs.x + rhs.x, lhs.y + rhs.y);
    }
    static inline ImVec2 operator-(const ImVec2& lhs, const ImVec2& rhs) {
        return ImVec2(lhs.x - rhs.x, lhs.y - rhs.y);
    }
    static inline ImVec2 operator*(const ImVec2& lhs, const ImVec2& rhs) {
        return ImVec2(lhs.x * rhs.x, lhs.y * rhs.y);
    }
    static inline ImVec2 operator/(const ImVec2& lhs, const ImVec2& rhs) {
        return ImVec2(lhs.x / rhs.x, lhs.y / rhs.y);
    }
    static inline ImVec2& operator+=(ImVec2& lhs, const ImVec2& rhs) {
        lhs.x += rhs.x;
        lhs.y += rhs.y;
        return lhs;
    }
    static inline ImVec2& operator-=(ImVec2& lhs, const ImVec2& rhs) {
        lhs.x -= rhs.x;
        lhs.y -= rhs.y;
        return lhs;
    }
    static inline ImVec2& operator*=(ImVec2& lhs, const float rhs) {
        lhs.x *= rhs;
        lhs.y *= rhs;
        return lhs;
    }
    static inline ImVec2& operator/=(ImVec2& lhs, const float rhs) {
        lhs.x /= rhs;
        lhs.y /= rhs;
        return lhs;
    }
}
