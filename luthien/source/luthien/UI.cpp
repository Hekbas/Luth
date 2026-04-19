#include "lepch.h"
#include "UI.h"
#include "luthien/widgets/Properties.h"
#include "luth/resources/AssetDatabase.h"
#include "luth/resources/AssetManager.h"
#include "luth/renderer/Renderer.h"
#include "luth/renderer/RenderBackend.h"
#include "luth/renderer/backend/vulkan/VulkanTexture.h"
#include "luth/renderer/backend/vulkan/VulkanContext.h"
#include "luthien/widgets/Icons.h"

#include <imgui.h>
#include <backends/imgui_impl_vulkan.h>


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
