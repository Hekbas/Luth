#pragma once

#include "luth/core/types/LuthMath.h"
#include "luth/core/UUID.h"
#include "luth/resources/Asset.h"
#include "luth/renderer/resources/Texture.h"

#include "luthien/widgets/CollapsingHeader.h"
#include "luthien/widgets/InfoTable.h"

#include <string>
#include <imgui.h>

namespace Luth::UI
{
    bool BeginProperties(const char* id = "Properties");
    void EndProperties();

    bool Property(const char* label, std::string& value);
    bool Property(const char* label, bool& value);
    bool Property(const char* label, int& value, int min = 0, int max = 0);
    bool Property(const char* label, float& value, float speed = 0.1f, float min = 0.0f, float max = 0.0f);

    // Vector widgets with per-axis reset buttons (X/Y/Z/W).
    bool Property(const char* label, Vec2& value, float speed = 0.1f, float resetValue = 0.0f);
    bool Property(const char* label, Vec3& value, float speed = 0.1f, float resetValue = 0.0f);
    bool Property(const char* label, Vec4& value, float speed = 0.1f, float resetValue = 0.0f);

    bool PropertyColor(const char* label, Vec3& value);
    bool PropertyColor(const char* label, Vec4& value);

    bool PropertyAsset(const char* label, UUID& assetHandle, AssetType type);

    bool PropertyCombo(const char* label, int& currentIndex, const char* const items[], int count);

    void TexturePreview(const std::shared_ptr<Texture>& texture, float maxWidth = 0.0f);

    // Allocates and caches a per-texture Vulkan descriptor set.
    ImTextureID GetTextureID(const std::shared_ptr<Texture>& texture);

    // Must run before ImGui_ImplVulkan_Shutdown to free cached descriptor sets.
    void ClearTextureCache();
}