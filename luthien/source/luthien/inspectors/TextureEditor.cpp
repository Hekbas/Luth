#include "lepch.h"
#include "luthien/inspectors/TextureEditor.h"
#include "luthien/Editor.h"
#include "luthien/EditorSettings.h"
#include "luthien/widgets/Widgets.h"
#include "luthien/widgets/ThumbnailCache.h"
#include "luthien/widgets/Icons.h"
#include "luth/renderer/resources/Texture.h"
#include "luth/resources/AssetDatabase.h"
#include "luth/resources/AssetManager.h"
#include "luth/resources/MetaFile.h"

#include <algorithm>

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

            // Read generate_mipmaps + channel role from .meta
            m_GenerateMipmaps = true;
            m_Role = 0;
            fs::path metaPath = texture.GetPath().string() + ".meta";
            MetaFile meta(texture.Handle);
            if (meta.Load(metaPath))
            {
                auto& ts = meta.GetTypeSettings();
                if (ts.contains("generate_mipmaps")) m_GenerateMipmaps = ts["generate_mipmaps"].get<bool>();
                if (ts.contains("role"))             m_Role = ts["role"].get<int>();
            }
        }

        // Header: thumbnail-on-left, name + dimensions on right.
        ImTextureID headerThumb = UI::ThumbnailCache::Get(texture.Handle, AssetType::Texture);
        UI::InspectorHeader(headerThumb, ICON_FA_IMAGE, 48.0f, [&]() {
            const ImVec4 nameCol = { 1.0f, 0.7f, 0.2f, 1.0f };
            ImGui::TextColored(nameCol, "%s (Texture)", texture.GetName().c_str());
            ImGui::TextDisabled("%dx%d  ·  %s  ·  %d mip%s",
                texture.GetWidth(), texture.GetHeight(),
                texture.GetFormatString().c_str(),
                texture.GetMipLevels(),
                texture.GetMipLevels() == 1 ? "" : "s");
        });

        ImGui::Dummy({ 0, 4 });

        // Pinned-footer layout: snapshot pattern — Settings + Preview size with
        // the same frame-start value; Splitter mutates the persisted height so
        // the change takes effect next frame (no one-frame overshoot).
        const float kSplitterH    = 4.0f;
        const float kMinSettingsH = 80.0f;
        const float kMinFooterH   = 80.0f;
        const float kMaxFooterAbs = 400.0f;
        const float spacingY      = ImGui::GetStyle().ItemSpacing.y;
        const float availH        = ImGui::GetContentRegionAvail().y;
        float& footerH = Editor::GetSettings().texturePreviewFooterHeight;
        const float kMaxFooterH = std::max(kMinFooterH,
            std::min(kMaxFooterAbs, availH - kMinSettingsH - kSplitterH - 2.0f * spacingY));
        footerH = std::clamp(footerH, kMinFooterH, kMaxFooterH);
        const float footerH_snap = footerH;
        const float topH = availH - footerH_snap - kSplitterH - 2.0f * spacingY;
        if (ImGui::BeginChild("##Settings", { -1, topH }, false))
        {
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
                // Order matches TextureRole (Color, NormalGL, NormalDX, LinearData, GlossToRoughness).
                const char* roleModes[] = { "Color", "Normal (GL)", "Normal (DX)", "Linear Data", "Gloss to Roughness" };

                if (UI::BeginProperties("TextureSettings")) {
                    UI::Property("Generate Mipmaps", m_GenerateMipmaps);
                    UI::PropertyCombo("Wrap Mode",  m_WrapMode,  wrapModes,   IM_ARRAYSIZE(wrapModes));
                    UI::PropertyCombo("Min Filter", m_MinFilter, filterModes, IM_ARRAYSIZE(filterModes));
                    UI::PropertyCombo("Mag Filter", m_MagFilter, filterModes, IM_ARRAYSIZE(filterModes));
                    UI::PropertyCombo("Channel Role", m_Role, roleModes, IM_ARRAYSIZE(roleModes));
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
                        ts["role"] = m_Role;
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
        }
        ImGui::EndChild();

        if (UI::Splitter("##TextureSplitter", &footerH, kMinFooterH, kMaxFooterH, kSplitterH))
            Editor::SaveSettings();

        if (ImGui::BeginChild("##Preview", { -1, footerH_snap }, false))
        {
            UI::TexturePreview(std::shared_ptr<Texture>(&texture, [](Texture*){}));
        }
        ImGui::EndChild();
    }
}
