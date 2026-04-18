#include "lepch.h"
#include "UI.h"
#include "luthien/Editor.h"
#include "luthien/EditorColors.h"
#include "luth/resources/AssetDatabase.h"
#include "luth/resources/AssetManager.h"
#include "luth/renderer/Renderer.h"
#include "luth/renderer/RenderBackend.h"
#include "luth/renderer/backend/vulkan/VulkanTexture.h"
#include "luth/renderer/backend/vulkan/VulkanContext.h"
#include "luthien/widgets/Icons.h"

#include <imgui.h>
#include <backends/imgui_impl_vulkan.h>
#include <imgui/imgui_internal.h>


namespace Luth::UI
{
    static void PushMultiItemsWidths(int components, float w_full)
    {
        const ImGuiStyle& style = ImGui::GetStyle();
        const float w_item_one = ImMax(1.0f, (w_full - (style.ItemInnerSpacing.x) * (components - 1)) / (float)components);
        for (int i = 0; i < components; i++)
            ImGui::PushItemWidth(w_item_one);
    }

    bool BeginCollapsingHeader(const char* label, bool defaultOpen, const std::function<void()>& contextMenu)
    {
        ImGuiWindow* window = ImGui::GetCurrentWindow();
        if (window->SkipItems) return false;

        ImGuiID id = window->GetID(label);
        bool is_open = window->StateStorage.GetInt(id, defaultOpen ? 1 : 0) != 0;

        ImVec2 pos = window->DC.CursorPos;
        ImVec2 size = ImVec2(ImGui::GetContentRegionAvail().x, ImGui::GetFrameHeight() + 8.0f);
        ImRect bb(pos, ImVec2(pos.x + size.x, pos.y + size.y));

        ImGui::ItemSize(bb);
        if (!ImGui::ItemAdd(bb, id))
        {
            if (is_open)
            {
                ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.15f, 0.15f, 0.15f, 1.0f));
                ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(0.24f, 0.24f, 0.24f, 1.0f));
                ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 4.0f);
                ImGui::PushStyleVar(ImGuiStyleVar_ChildBorderSize, 1.0f);
                ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(8.0f, 8.0f));
                
                char child_id[128];
                snprintf(child_id, sizeof(child_id), "##%s_content", label);
                ImGui::BeginChild(child_id, ImVec2(0, 0), ImGuiChildFlags_Borders | ImGuiChildFlags_AutoResizeY | ImGuiChildFlags_AlwaysUseWindowPadding);
            }
            return is_open;
        }

        bool hovered, held;
        bool pressed = ImGui::ButtonBehavior(bb, id, &hovered, &held);
        if (pressed)
        {
            is_open = !is_open;
            window->StateStorage.SetInt(id, is_open ? 1 : 0);
        }

        if (contextMenu)
        {
            std::string popupId = "HeaderContextMenu_" + std::string(label);
            if (hovered && ImGui::IsMouseReleased(ImGuiMouseButton_Right))
                ImGui::OpenPopup(popupId.c_str());

            if (ImGui::BeginPopup(popupId.c_str()))
            {
                contextMenu();
                ImGui::EndPopup();
            }
        }

        ImU32 bg_col;
        if (is_open) 
            bg_col = ImGui::GetColorU32(ImVec4(0.24f, 0.24f, 0.24f, 1.0f));
        else if (hovered)
            bg_col = ImGui::GetColorU32(ImGuiCol_HeaderHovered);
        else
            bg_col = ImGui::GetColorU32(ImGuiCol_Header);

        window->DrawList->AddRectFilled(bb.Min, bb.Max, bg_col, 4.0f, is_open ? ImDrawFlags_RoundCornersTop : ImDrawFlags_RoundCornersAll);

        ImGui::PushFont(Editor::GetFASolid());
        const char* icon = is_open ? ICON_FA_CARET_DOWN : ICON_FA_CARET_RIGHT;
        ImVec2 icon_size = ImGui::CalcTextSize(icon);
        ImVec2 icon_pos = ImVec2(bb.Min.x + 8.0f, bb.Min.y + (size.y - icon_size.y) * 0.5f);
        window->DrawList->AddText(icon_pos, ImGui::GetColorU32(ImGuiCol_Text), icon);
        ImGui::PopFont();

        ImVec2 text_size = ImGui::CalcTextSize(label);
        ImVec2 text_pos = ImVec2(bb.Min.x + 28.0f, bb.Min.y + (size.y - text_size.y) * 0.5f);
        window->DrawList->AddText(text_pos, ImGui::GetColorU32(ImGuiCol_Text), label);

        float handle_height = 10.0f;
        ImVec2 handle_p = ImVec2(bb.Max.x - 16.0f, bb.Min.y + (size.y - handle_height) * 0.5f);
        ImU32 dot_color = ImGui::GetColorU32(ImGuiCol_TextDisabled);
        for (int x = 0; x < 2; x++)
        {
            for (int y = 0; y < 4; y++)
            {
                window->DrawList->AddRectFilled(
                    ImVec2(handle_p.x + x * 4.0f, handle_p.y + y * 3.0f),
                    ImVec2(handle_p.x + x * 4.0f + 2.0f, handle_p.y + y * 3.0f + 2.0f),
                    dot_color
                );
            }
        }

        if (is_open)
        {
            ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.15f, 0.15f, 0.15f, 1.0f));
            ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(0.24f, 0.24f, 0.24f, 1.0f));
            ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 4.0f);
            ImGui::PushStyleVar(ImGuiStyleVar_ChildBorderSize, 1.0f);
            ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(8.0f, 8.0f));
            
            ImGui::SetCursorPosY(ImGui::GetCursorPosY() - ImGui::GetStyle().ItemSpacing.y);
            
            char child_id[128];
            snprintf(child_id, sizeof(child_id), "##%s_content", label);
            ImGui::BeginChild(child_id, ImVec2(0, 0), ImGuiChildFlags_Borders | ImGuiChildFlags_AutoResizeY | ImGuiChildFlags_AlwaysUseWindowPadding);
        }

        return is_open;
    }

    void EndCollapsingHeader()
    {
        ImGui::EndChild();
        ImGui::PopStyleVar(3);
        ImGui::PopStyleColor(2);
        ImGui::Spacing();
    }

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

    // Helper to draw label column and setup value column
    static void DrawLabel(const char* label)
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
        DrawLabel(label);
        
        char buffer[256];
        memset(buffer, 0, sizeof(buffer));
        strncpy_s(buffer, value.c_str(), sizeof(buffer));

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
        DrawLabel(label);
        std::string id = "##" + std::string(label);
        bool changed = ImGui::Checkbox(id.c_str(), &value);
        ImGui::PopItemWidth();
        return changed;
    }

    bool Property(const char* label, int& value, int min, int max)
    {
        DrawLabel(label);
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
        DrawLabel(label);
        std::string id = "##" + std::string(label);
        bool changed = ImGui::DragFloat(id.c_str(), &value, speed, min, max, "%.3f");
        ImGui::PopItemWidth();
        return changed;
    }

    // Internal helper for Vec3/Vec2/Vec4
    static bool DrawVecControl(const char* label, float* values, int components, float resetValue, float speed)
    {
        DrawLabel(label);
        
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
        ImGui::PopItemWidth(); // Pop the one pushed in DrawLabel
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
        DrawLabel(label);
        std::string id = "##" + std::string(label);
        bool changed = ImGui::ColorEdit3(id.c_str(), &value.x);
        ImGui::PopItemWidth();
        return changed;
    }

    bool PropertyColor(const char* label, Vec4& value)
    {
        DrawLabel(label);
        std::string id = "##" + std::string(label);
        bool changed = ImGui::ColorEdit4(id.c_str(), &value.x);
        ImGui::PopItemWidth();
        return changed;
    }

    bool PropertyAsset(const char* label, UUID& assetHandle, AssetType type)
    {
        DrawLabel(label);

        bool changed = false;
        std::string assetName = "None";
        
        // Try to get asset name if handle is valid
        if (assetHandle.IsValid())
        {
            if (AssetManager::IsLoaded(assetHandle))
            {
                auto asset = AssetManager::GetAsset<Asset>(assetHandle);
                if (asset) assetName = asset->GetName();
            }
            else
            {
                // Fallback to metadata if not loaded
                const auto& meta = AssetDatabase::GetMetadata(assetHandle);
                if (!meta.Path.empty())
                    assetName = meta.Path.filename().string();
                else
                    assetName = "Invalid Asset";
            }
        }

        // Button to show current asset
        std::string id = "##" + std::string(label);
        ImGui::PushStyleVar(ImGuiStyleVar_ButtonTextAlign, ImVec2(0.0f, 0.5f));
        
        // Calculate icon based on type
        const char* icon = ICON_FA_FILE;
        switch(type) {
            case AssetType::Model: icon = ICON_FA_CUBE; break;
            case AssetType::Texture: icon = ICON_FA_IMAGE; break;
            case AssetType::Material: icon = ICON_FA_CIRCLE_HALF_STROKE; break;
            case AssetType::Shader: icon = ICON_FA_FILE_CODE; break;
            default: break;
        }

        std::string buttonText = std::string(icon) + "  " + assetName;
        if (ImGui::Button(buttonText.c_str(), ImVec2(-1, 0)))
        {
            // TODO: Open Asset Picker popup
        }
        ImGui::PopStyleVar();

        // Drag & Drop Target
        if (ImGui::BeginDragDropTarget())
        {
            if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("ASSET_UUID"))
            {
                const UUID* droppedUUID = static_cast<const UUID*>(payload->Data);
                
                // Validate type
                const auto& meta = AssetDatabase::GetMetadata(*droppedUUID);
                if (meta.Type == type)
                {
                    assetHandle = *droppedUUID;
                    changed = true;
                }
                else
                {
                    LH_CORE_WARN("Invalid asset type dropped. Expected {0}, got {1}", (int)type, (int)meta.Type);
                }
            }
            ImGui::EndDragDropTarget();
        }

        // Right click to clear
        if (ImGui::BeginPopupContextItem())
        {
            if (ImGui::MenuItem("Clear"))
            {
                assetHandle = UUID::Invalid();
                changed = true;
            }
            ImGui::EndPopup();
        }

        ImGui::PopItemWidth();
        return changed;
    }

    bool PropertyCombo(const char* label, int& currentIndex, const char* const items[], int count)
    {
        DrawLabel(label);
        std::string id = "##" + std::string(label);
        bool changed = ImGui::Combo(id.c_str(), &currentIndex, items, count);
        ImGui::PopItemWidth();
        return changed;
    }

    bool BeginInfoTable(const char* id)
    {
        return ImGui::BeginTable(id, 2, ImGuiTableFlags_SizingStretchSame);
    }

    void InfoRow(const char* label, const char* fmt, ...)
    {
        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex(0);
        ImGui::Text("%s", label);
        ImGui::TableSetColumnIndex(1);

        va_list args;
        va_start(args, fmt);
        ImGui::TextV(fmt, args);
        va_end(args);
    }

    void EndInfoTable()
    {
        ImGui::EndTable();
    }

    void TexturePreview(const std::shared_ptr<Texture>& texture, float maxWidth)
    {
        if (!texture) return;

        float imageAR = (float)texture->GetHeight() / (float)texture->GetWidth();
        float availWidth = maxWidth > 0.0f ? maxWidth : ImGui::GetContentRegionAvail().x;
        float availHeight = ImGui::GetContentRegionAvail().y;
        float availAR = availHeight / availWidth;
        float previewWidth, previewHeight;
        ImVec2 offset;

        if (availAR > 1.0f) {
            previewWidth = availWidth;
            previewHeight = previewWidth * imageAR;
            offset = { 0, (availHeight - previewHeight) / 2.0f };
        }
        else {
            previewHeight = availHeight;
            previewWidth = previewHeight / imageAR;
            offset = { (availWidth - previewWidth) / 2.0f, 0 };
        }

        if (ImGui::BeginChild("PreviewRegion", { availWidth, 0 })) {
            ImGui::SetCursorPos(offset);
            ImGui::Image(GetTextureID(texture), { previewWidth, previewHeight }, { 0, 0 }, { 1, 1 });
        }
        ImGui::EndChild();
    }

    // File-scope so ClearTextureCache() can access it
    static std::unordered_map<void*, std::pair<VkDescriptorSet, std::weak_ptr<Texture>>> s_TextureCache;

    ImTextureID GetTextureID(const std::shared_ptr<Texture>& texture)
    {
        if (!texture) return 0;

        if (Renderer::GetBackend()->GetAPI() == RenderBackend::API::Vulkan)
        {
            // Cleanup stale entries
            for (auto it = s_TextureCache.begin(); it != s_TextureCache.end(); )
            {
                if (it->second.second.expired())
                {
                    VkDescriptorSet set = it->second.first;
                    VulkanContext::Get().PushDeletion([set]() {
                        ImGui_ImplVulkan_RemoveTexture(set);
                    });
                    it = s_TextureCache.erase(it);
                }
                else
                {
                    ++it;
                }
            }

            void* key = texture.get();
            if (s_TextureCache.find(key) == s_TextureCache.end())
            {
                auto vkTex = std::static_pointer_cast<VKTexture>(texture);
                VkDescriptorSet set = ImGui_ImplVulkan_AddTexture(
                    vkTex->GetSampler(),
                    vkTex->GetImageView(),
                    VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
                s_TextureCache[key] = { set, texture };
            }
            return (ImTextureID)s_TextureCache[key].first;
        }

        return (ImTextureID)(uintptr_t)texture->GetRendererID();
    }

    void ClearTextureCache()
    {
        for (auto& [key, entry] : s_TextureCache) {
            ImGui_ImplVulkan_RemoveTexture(entry.first);
        }
        s_TextureCache.clear();
    }
}
