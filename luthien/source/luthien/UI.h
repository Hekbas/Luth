#pragma once

#include "luth/renderer/resources/Texture.h"

#include "luthien/widgets/AssetSlot.h"
#include "luthien/widgets/CollapsingHeader.h"
#include "luthien/widgets/InfoTable.h"
#include "luthien/widgets/Properties.h"

#include <imgui.h>

namespace Luth::UI
{
    void TexturePreview(const std::shared_ptr<Texture>& texture, float maxWidth = 0.0f);

    // Allocates and caches a per-texture Vulkan descriptor set.
    ImTextureID GetTextureID(const std::shared_ptr<Texture>& texture);

    // Must run before ImGui_ImplVulkan_Shutdown to free cached descriptor sets.
    void ClearTextureCache();
}