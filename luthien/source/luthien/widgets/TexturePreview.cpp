#include "lepch.h"
#include "luthien/widgets/TexturePreview.h"
#include "luth/renderer/Renderer.h"
#include "luth/renderer/RenderBackend.h"
#include "luth/renderer/backend/vulkan/VulkanTexture.h"
#include "luth/renderer/backend/vulkan/VulkanContext.h"

#include <imgui.h>
#include <backends/imgui_impl_vulkan.h>


namespace Luth::UI
{
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
