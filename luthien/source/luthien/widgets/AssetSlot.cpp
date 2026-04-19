#include "lepch.h"
#include "luthien/widgets/AssetSlot.h"
#include "luthien/widgets/Properties.h"
#include "luthien/widgets/Icons.h"
#include "luth/resources/AssetDatabase.h"
#include "luth/resources/AssetManager.h"

#include <imgui.h>


namespace Luth::UI
{
    bool PropertyAsset(const char* label, UUID& assetHandle, AssetType type)
    {
        PropertyLabel(label);

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
}
