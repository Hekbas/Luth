#include "luthpch.h"
#include "luth/editor/panels/TextureRemapDialog.h"
#include "luth/editor/EditorColors.h"
#include "luth/editor/Editor.h"
#include "luth/resources/AssetDatabase.h"
#include "luth/resources/MetaFile.h"
#include "luth/resources/importers/TextureResolver.h"
#include "luth/platform/FileDialog.h"
#include "luth/renderer/Material.h"

#include "luth/editor/widgets/Icons.h"

#include <imgui.h>
#include <nlohmann/json.hpp>
#include <fstream>

namespace Luth
{
    bool             TextureRemapDialog::s_Open = false;
    ImportReport     TextureRemapDialog::s_Report;
    std::vector<std::string> TextureRemapDialog::s_UserPaths;
    char             TextureRemapDialog::s_SearchDir[512] = {};

    // ── Public API ─────────────────────────────────────────────────────────────

    void TextureRemapDialog::Open(const ImportReport& report)
    {
        s_Report = report;
        s_UserPaths.assign(report.Unresolved.size(), "");
        s_SearchDir[0] = '\0';
        s_Open = true;
        ImGui::OpenPopup("##TextureRemap");
    }

    bool TextureRemapDialog::Draw()
    {
        if (!s_Open) return false;

        bool closed = false;

        ImGui::SetNextWindowSize(ImVec2(680, 0), ImGuiCond_Appearing);
        ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));

        if (ImGui::BeginPopupModal("##TextureRemap", nullptr,
            ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_AlwaysAutoResize))
        {
            // --- Title bar ---
            ImGui::PushFont(Editor::GetFASolid());
            ImGui::TextColored(EditorColors::WarningYellow, ICON_FA_TRIANGLE_EXCLAMATION "  Missing Textures");
            ImGui::PopFont();
            ImGui::SameLine(ImGui::GetContentRegionAvail().x - ImGui::CalcTextSize(s_Report.ModelPath.filename().string().c_str()).x);
            ImGui::TextDisabled("%s", s_Report.ModelPath.filename().string().c_str());
            ImGui::Separator();

            ImGui::Spacing();
            ImGui::TextWrapped(
                "The following textures could not be found automatically.\n"
                "Use Browse to pick individual files, or set a Search Directory to resolve all at once.");
            ImGui::Spacing();
            ImGui::Separator();
            ImGui::Spacing();

            // --- Per-texture rows ---
            std::string lastMat;
            for (int i = 0; i < (int)s_Report.Unresolved.size(); ++i)
            {
                auto& entry = s_Report.Unresolved[i];
                auto& userPath = s_UserPaths[i];

                // Material group header
                if (entry.MaterialName != lastMat) {
                    lastMat = entry.MaterialName;
                    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.75f, 0.75f, 0.75f, 1.0f));
                    ImGui::Text("Material:  %s", entry.MaterialName.c_str());
                    ImGui::PopStyleColor();
                }

                ImGui::Indent(16.0f);

                // Type badge
                ImGui::TextColored(EditorColors::WarningYellow, "[%s]", Material::ToString(entry.Type));
                ImGui::SameLine();

                // Original path (dim)
                ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.5f, 0.5f, 0.5f, 1.0f));
                ImGui::TextUnformatted(entry.OriginalPath.c_str());
                ImGui::PopStyleColor();

                // Resolved / user-provided path
                if (!userPath.empty()) {
                    ImGui::SameLine();
                    ImGui::TextColored(EditorColors::SuccessGreen, ICON_FA_CHECK " %s",
                        fs::path(userPath).filename().string().c_str());
                }

                // Browse button
                ImGui::SameLine(ImGui::GetContentRegionAvail().x - 60.0f);
                ImGui::PushID(i);
                if (ImGui::Button("Browse", ImVec2(60, 0))) {
                    auto picked = FileDialog::OpenFile("Image Files (*.png *.jpg *.jpeg *.tga)\0*.png;*.jpg;*.jpeg;*.tga\0All Files\0*.*\0");
                    if (picked)
                        userPath = picked->string();
                }
                ImGui::PopID();

                ImGui::Unindent(16.0f);
            }

            ImGui::Spacing();
            ImGui::Separator();
            ImGui::Spacing();

            // --- Search-directory shortcut ---
            ImGui::AlignTextToFramePadding();
            ImGui::TextUnformatted("Search Directory:");
            ImGui::SameLine();
            ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - 70.0f);
            ImGui::InputText("##SearchDir", s_SearchDir, sizeof(s_SearchDir));
            ImGui::SameLine();
            if (ImGui::Button("Browse##dir", ImVec2(60, 0))) {
                // Reuse OpenFile in a directory-pick fashion by letting the user type the path
                // (native folder picker requires platform extension; use file picker for now)
                auto picked = FileDialog::OpenFile("All Files\0*.*\0");
                if (picked) {
                    fs::path dir = picked->parent_path();
                    strncpy_s(s_SearchDir, sizeof(s_SearchDir), dir.string().c_str(), _TRUNCATE);
                }
            }

            // Auto-resolve button
            if (ImGui::Button("Resolve All from Directory")) {
                fs::path searchDir(s_SearchDir);
                if (fs::exists(searchDir) && fs::is_directory(searchDir)) {
                    for (int i = 0; i < (int)s_Report.Unresolved.size(); ++i) {
                        if (!s_UserPaths[i].empty()) continue; // already set
                        auto& entry = s_Report.Unresolved[i];
                        ResolveResult r = ResolveTexturePath(searchDir, fs::path(entry.OriginalPath).filename().string());
                        if (!r.ResolvedPath.empty())
                            s_UserPaths[i] = r.ResolvedPath.string();
                    }
                }
            }

            ImGui::Spacing();
            ImGui::Separator();
            ImGui::Spacing();

            // --- Action buttons ---
            float btnW = 110.0f;
            float totalW = btnW * 2 + ImGui::GetStyle().ItemSpacing.x;
            ImGui::SetCursorPosX((ImGui::GetContentRegionAvail().x - totalW) * 0.5f + ImGui::GetStyle().WindowPadding.x);

            bool anyProvided = false;
            for (auto& p : s_UserPaths) if (!p.empty()) { anyProvided = true; break; }

            if (!anyProvided) ImGui::BeginDisabled();
            if (ImGui::Button("Apply", ImVec2(btnW, 0))) {
                ApplyResolutions();
                s_Open = false;
                closed = true;
                ImGui::CloseCurrentPopup();
            }
            if (!anyProvided) ImGui::EndDisabled();

            ImGui::SameLine();
            if (ImGui::Button("Skip", ImVec2(btnW, 0))) {
                s_Open = false;
                closed = true;
                ImGui::CloseCurrentPopup();
            }

            ImGui::EndPopup();
        }

        return closed;
    }

    // ── Private ────────────────────────────────────────────────────────────────

    void TextureRemapDialog::ApplyResolutions()
    {
        for (int i = 0; i < (int)s_Report.Unresolved.size(); ++i)
        {
            const std::string& userPath = s_UserPaths[i];
            if (userPath.empty()) continue;

            fs::path texPath(userPath);
            if (!fs::exists(texPath)) continue;

            auto& entry = s_Report.Unresolved[i];

            // Register texture in AssetDatabase if not already known
            UUID texUUID = AssetDatabase::GetUUID(texPath);
            if (!texUUID.IsValid()) {
                texUUID = MetaFile::Create(texPath, AssetType::Texture);
                AssetDatabase::RegisterAsset(texPath, texUUID, AssetType::Texture);
            }

            // Patch the .mat JSON on disk
            if (!fs::exists(entry.MaterialPath)) continue;

            nlohmann::json matJson;
            {
                std::ifstream f(entry.MaterialPath);
                if (!f.is_open()) continue;
                f >> matJson;
            }

            // Check if this type already has an entry; if so, update uuid, else append
            bool found = false;
            int typeInt = (int)entry.Type;
            for (auto& texNode : matJson["textures"]) {
                if (texNode["type"].get<int>() == typeInt) {
                    texNode["uuid"] = texUUID.ToString();
                    texNode["useTexture"] = true;
                    found = true;
                    break;
                }
            }
            if (!found) {
                nlohmann::json texNode;
                texNode["type"] = typeInt;
                texNode["uuid"] = texUUID.ToString();
                texNode["uv"] = 0;
                texNode["useTexture"] = true;
                matJson["textures"].push_back(texNode);
            }

            {
                std::ofstream f(entry.MaterialPath);
                if (!f.is_open()) continue;
                f << matJson.dump(4);
            }

            // Invalidate the material's artifact so it gets reimported with the new texture
            UUID matUUID = AssetDatabase::GetUUID(entry.MaterialPath);
            if (matUUID.IsValid()) {
                fs::path artifact = AssetDatabase::GetArtifactPath(matUUID);
                if (fs::exists(artifact)) fs::remove(artifact);
            }

            LH_CORE_INFO("TextureRemapDialog: Assigned '{}' -> material '{}' ({})",
                texPath.filename().string(), entry.MaterialName, Material::ToString(entry.Type));
        }
    }
}
