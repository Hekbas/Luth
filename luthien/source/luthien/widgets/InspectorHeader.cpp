#include "lepch.h"
#include "luthien/widgets/InspectorHeader.h"

namespace Luth::UI
{
    void InspectorHeader(ImTextureID thumb, const char* fallbackIcon,
                         float thumbSize, std::function<void()> rightContent)
    {
        const float pad = 8.0f;
        const float headerH = thumbSize + pad * 2;

        if (ImGui::BeginChild("##InspectorHeader", { 0, headerH },
                              false, ImGuiWindowFlags_NoScrollbar))
        {
            // Anchor inside child with both top + left padding. Dummy+SameLine
            // can give one or the other but not both — SetCursorPos avoids that.
            ImGui::SetCursorPos({ pad, pad });

            const ImVec2 boxStart = ImGui::GetCursorScreenPos();
            if (thumb) {
                ImGui::Image(thumb, { thumbSize, thumbSize });
            } else {
                ImGui::Dummy({ thumbSize, thumbSize });
                if (fallbackIcon && *fallbackIcon) {
                    const ImVec2 ts = ImGui::CalcTextSize(fallbackIcon);
                    const ImVec2 iconPos = {
                        boxStart.x + (thumbSize - ts.x) * 0.5f,
                        boxStart.y + (thumbSize - ts.y) * 0.5f
                    };
                    ImGui::GetWindowDrawList()->AddText(
                        iconPos,
                        ImGui::GetColorU32(ImGuiCol_TextDisabled),
                        fallbackIcon);
                }
            }

            ImGui::SameLine(0.0f, pad);
            ImGui::BeginGroup();
            if (rightContent) rightContent();
            ImGui::EndGroup();
        }
        ImGui::EndChild();
    }
}
