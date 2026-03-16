#pragma once

#include "luth/core/LuthTypes.h"
#include "luth/core/UUID.h"
#include "luth/resources/Asset.h"
#include "luth/renderer/Texture.h"

#include <string>
#include <glm/glm.hpp>
#include <imgui.h>

namespace Luth::UI
{
    // Layout
    void BeginProperties(const char* id = "Properties");
    void EndProperties();

    // Widgets
    bool Property(const char* label, std::string& value);
    bool Property(const char* label, bool& value);
    bool Property(const char* label, int& value, int min = 0, int max = 0);
    bool Property(const char* label, float& value, float speed = 0.1f, float min = 0.0f, float max = 0.0f);
    
    // Vector widgets with reset buttons (X/Y/Z/W)
    bool Property(const char* label, Vec2& value, float speed = 0.1f, float resetValue = 0.0f);
    bool Property(const char* label, Vec3& value, float speed = 0.1f, float resetValue = 0.0f);
    bool Property(const char* label, Vec4& value, float speed = 0.1f, float resetValue = 0.0f);
    
    bool PropertyColor(const char* label, Vec3& value);
    bool PropertyColor(const char* label, Vec4& value);

    // Asset Slot with Drag & Drop support
    bool PropertyAsset(const char* label, UUID& assetHandle, AssetType type);

    // Helper to get ImGui Texture ID (Handles Vulkan Descriptor creation/caching)
    ImTextureID GetTextureID(const std::shared_ptr<Texture>& texture);

    // Must be called during shutdown before ImGui_ImplVulkan_Shutdown
    void ClearTextureCache();
}