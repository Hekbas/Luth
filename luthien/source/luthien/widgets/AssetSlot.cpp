#include "lepch.h"
#include "luthien/widgets/AssetSlot.h"
#include "luthien/widgets/Icons.h"
#include "luthien/widgets/ThumbnailCache.h"
#include "luth/resources/AssetDatabase.h"
#include "luth/resources/AssetManager.h"

#include <imgui.h>


namespace Luth::UI
{
    EditState PropertyAsset(const char* label, UUID& assetHandle, AssetType type)
    {
        PropertyLabel(label);

        UUID pre = assetHandle;
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
            case AssetType::Animation: icon = ICON_FA_PERSON_RUNNING; break;
            default: break;
        }

        // Asset-type thumbnail overrides the icon when baked. Hit/miss = ImTextureID/0.
        const bool thumbCapable = assetHandle.IsValid() &&
            (type == AssetType::Material || type == AssetType::Model || type == AssetType::Texture);
        const ImTextureID thumb = thumbCapable ? ThumbnailCache::Get(assetHandle, type) : (ImTextureID)0;

        // Reserve space at the button's left edge for the thumbnail overlay (drawn
        // after the button so it composites on top of the button background).
        std::string buttonText = thumb
            ? std::string("    ") + assetName
            : std::string(icon) + "  " + assetName;
        if (ImGui::Button(buttonText.c_str(), ImVec2(-1, 0)))
        {
            // TODO: Open Asset Picker popup
        }
        ImGuiID itemId = ImGui::GetItemID();
        if (thumb) {
            ImVec2 itemMin = ImGui::GetItemRectMin();
            ImVec2 itemMax = ImGui::GetItemRectMax();
            float pad = 2.0f;
            float sz = (itemMax.y - itemMin.y) - 2 * pad;
            ImGui::GetWindowDrawList()->AddImage(thumb,
                { itemMin.x + pad, itemMin.y + pad },
                { itemMin.x + pad + sz, itemMin.y + pad + sz });
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
                    LH_LOG(Editor, warn, "Invalid asset type dropped. Expected {0}, got {1}", (int)type, (int)meta.Type);
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

        EditState st{ changed, false, itemId };
        // Drag-drop and clear are discrete one-frame gestures: commit synchronously
        // and stash pre-value under the button's item ID for the caller to consume.
        if (changed) {
            SaveItemPreEdit<UUID>(itemId, pre);
            st.committed = true;
        }
        return st;
    }
}
