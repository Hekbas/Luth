#include "lepch.h"
#include "luthien/inspectors/TextureEditor.h"
#include "luthien/widgets/Widgets.h"
#include "luthien/widgets/ThumbnailCache.h"
#include "luthien/widgets/Icons.h"
#include "luth/renderer/resources/Texture.h"
#include "luth/resources/AssetDatabase.h"
#include "luth/resources/AssetManager.h"
#include "luth/resources/MetaFile.h"

namespace Luth
{
    void TextureEditor::Draw(Texture& texture)
    {
        // Per-texture state: reset combos when the selected texture changes
        if (texture.Handle != m_LastTextureUUID)
        {
            m_LastTextureUUID = texture.Handle;
            m_WrapMode = (int)texture.GetWrapMode();
            m_MinFilter = (int)texture.GetFilterMode().first;
            m_MagFilter = (int)texture.GetFilterMode().second;

            // Read generate_mipmaps from .meta
            m_GenerateMipmaps = true;
            fs::path metaPath = texture.GetPath().string() + ".meta";
            MetaFile meta(texture.Handle);
            if (meta.Load(metaPath))
            {
                auto& ts = meta.GetTypeSettings();
                if (ts.contains("generate_mipmaps")) m_GenerateMipmaps = ts["generate_mipmaps"].get<bool>();
            }
        }

        // Header: thumbnail-on-left, name + dimensions on right.
        ImTextureID headerThumb = UI::ThumbnailCache::Get(texture.Handle, AssetType::Texture);
        UI::InspectorHeader(headerThumb, ICON_FA_IMAGE, 64.0f, [&]() {
            const ImVec4 nameCol = { 1.0f, 0.7f, 0.2f, 1.0f };
            ImGui::TextColored(nameCol, "%s (Texture)", texture.GetName().c_str());
            ImGui::TextDisabled("%dx%d  ·  %s  ·  %d mip%s",
                texture.GetWidth(), texture.GetHeight(),
                texture.GetFormatString().c_str(),
                texture.GetMipLevels(),
                texture.GetMipLevels() == 1 ? "" : "s");
        });

        ImGui::Dummy({ 0, 4 });

        // ---- Info Section ----
        if (UI::BeginCollapsingHeader("Texture Info", true))
        {
            if (UI::BeginInfoTable("TextureProps")) {
                UI::InfoRow("Dimensions", "%d x %d", texture.GetWidth(), texture.GetHeight());
                UI::InfoRow("Format",     "%s", texture.GetFormatString().c_str());
                UI::InfoRow("Type",       "%s", "2D");
                UI::InfoRow("Mip Levels", "%d", texture.GetMipLevels());
                UI::EndInfoTable();
            }
            UI::EndCollapsingHeader();
        }

        ImGui::Dummy({ 0, 8 });
        
        // ---- Settings Section ----
        if (UI::BeginCollapsingHeader("Import Settings", true))
        {
            const char* wrapModes[] = { "Repeat", "Clamp to Edge", "Mirrored Repeat" };
            const char* filterModes[] = { "Linear", "Nearest", "Linear Mipmap", "Nearest Mipmap" };

            if (UI::BeginProperties("TextureSettings")) {
                UI::Property("Generate Mipmaps", m_GenerateMipmaps);
                UI::PropertyCombo("Wrap Mode",  m_WrapMode,  wrapModes,   IM_ARRAYSIZE(wrapModes));
                UI::PropertyCombo("Min Filter", m_MinFilter, filterModes, IM_ARRAYSIZE(filterModes));
                UI::PropertyCombo("Mag Filter", m_MagFilter, filterModes, IM_ARRAYSIZE(filterModes));
                UI::EndProperties();
            }

            ImGui::Dummy({ 0, 4 });

            // Apply button — right-aligned
            float buttonWidth = ImGui::CalcTextSize("Apply").x + ImGui::GetStyle().FramePadding.x * 2.0f;
            ImGui::SetCursorPosX(ImGui::GetContentRegionAvail().x - buttonWidth);
            if (ImGui::Button("Apply")) {
                fs::path metaPath = texture.GetPath().string() + ".meta";
                MetaFile meta(texture.Handle);
                if (meta.Load(metaPath))
                {
                    auto& ts = meta.GetTypeSettings();
                    ts["generate_mipmaps"] = m_GenerateMipmaps;
                    ts["wrap_mode"] = m_WrapMode;
                    ts["filter_min"] = m_MinFilter;
                    ts["filter_mag"] = m_MagFilter;
                    meta.Save(metaPath);

                    // Delete artifact to force reimport with new settings
                    fs::path artifactPath = AssetDatabase::GetArtifactPath(texture.Handle);
                    if (fs::exists(artifactPath))
                        fs::remove(artifactPath);

                    AssetManager::Import(texture.Handle);
                    AssetManager::Evict(texture.Handle);
                    m_LastTextureUUID = UUID::Invalid();
                }
            }
            UI::EndCollapsingHeader();
        }

        ImGui::Dummy({ 0, 4 });

        // ---- Preview Section ----
        UI::TexturePreview(std::shared_ptr<Texture>(&texture, [](Texture*){}));
    }
}
