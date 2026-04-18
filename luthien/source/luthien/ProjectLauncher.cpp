#include "lepch.h"
#include "luthien/ProjectLauncher.h"
#include "luthien/Editor.h"
#include "luthien/EditorColors.h"
#include "luth/core/ProjectFile.h"
#include "luth/core/Version.h"
#include "luth/resources/FileSystem.h"
#include "luth/platform/FileDialog.h"
#include "luthien/widgets/Icons.h"
#include "luthien/widgets/ImGuiUtils.h"

#include <imgui.h>
#include <nlohmann/json.hpp>
#include <fstream>
#include <chrono>

namespace Luth
{
    namespace fs = std::filesystem;

    // ================================================================
    // Time Helpers
    // ================================================================

    static int64_t NowUnix()
    {
        return std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
    }

    static std::string RelativeTimeString(int64_t timestamp)
    {
        if (timestamp <= 0) return "";

        int64_t delta = NowUnix() - timestamp;
        if (delta < 0) delta = 0;

        if (delta < 60)        return "Just now";
        if (delta < 3600)      return std::to_string(delta / 60) + " min ago";
        if (delta < 86400)     return std::to_string(delta / 3600) + " hours ago";
        if (delta < 604800)    return std::to_string(delta / 86400) + " days ago";
        if (delta < 2592000)   return std::to_string(delta / 604800) + " weeks ago";
        return std::to_string(delta / 2592000) + " months ago";
    }

    // ================================================================
    // Lifecycle
    // ================================================================

    void ProjectLauncher::Init()
    {
        LoadRecentProjects();
    }

    void ProjectLauncher::Show()  { s_Visible = true; }
    void ProjectLauncher::Hide()  { s_Visible = false; }
    bool ProjectLauncher::IsVisible() { return s_Visible; }

    bool ProjectLauncher::HasPendingProject() { return s_HasPending; }

    fs::path ProjectLauncher::ConsumePendingProject()
    {
        s_HasPending = false;
        return std::move(s_PendingProject);
    }

    // ================================================================
    // Recent Projects Persistence
    // ================================================================

    fs::path ProjectLauncher::GetConfigPath()
    {
        #ifdef _WIN32
        const char* appdata = std::getenv("APPDATA");
        if (appdata)
        {
            fs::path dir = fs::path(appdata) / "Luth";
            fs::create_directories(dir);
            return dir / "recent_projects.json";
        }
        #endif
        return FileSystem::EnginePath("recent_projects.json");
    }

    void ProjectLauncher::LoadRecentProjects()
    {
        fs::path path = GetConfigPath();
        if (!fs::exists(path)) return;

        try
        {
            std::ifstream file(path);
            nlohmann::json j = nlohmann::json::parse(file);

            s_RecentProjects.clear();
            for (auto& entry : j["projects"])
            {
                RecentProject rp;
                rp.Name       = entry.value("name", "Untitled");
                rp.Path       = entry.value("path", "");
                rp.Version    = entry.value("version", Luth::GetVersionString());
                rp.LastOpened = entry.value("lastOpened", (int64_t)0);
                if (!rp.Path.empty() && fs::exists(rp.Path))
                    s_RecentProjects.push_back(rp);
            }
        }
        catch (const std::exception& e)
        {
            LH_CORE_WARN("ProjectLauncher: Failed to load recent projects: {}", e.what());
        }
    }

    void ProjectLauncher::SaveRecentProjects()
    {
        try
        {
            nlohmann::json j;
            j["projects"] = nlohmann::json::array();
            for (auto& rp : s_RecentProjects)
            {
                nlohmann::json entry;
                entry["name"]       = rp.Name;
                entry["path"]       = rp.Path;
                entry["version"]    = rp.Version;
                entry["lastOpened"] = rp.LastOpened;
                j["projects"].push_back(entry);
            }

            fs::path path = GetConfigPath();
            std::ofstream file(path);
            file << j.dump(4);
        }
        catch (const std::exception& e)
        {
            LH_CORE_ERROR("ProjectLauncher: Failed to save recent projects: {}", e.what());
        }
    }

    void ProjectLauncher::AddRecent(const std::string& name, const fs::path& luthprojPath)
    {
        std::string absPath = fs::absolute(luthprojPath).string();

        // Try to read version from the .luthproj file
        std::string version = Luth::GetVersionString();
        try {
            if (fs::exists(luthprojPath)) {
                std::ifstream f(luthprojPath);
                nlohmann::json pj = nlohmann::json::parse(f);
                version = pj.value("version", Luth::GetVersionString());
            }
        } catch (...) {}

        // Remove existing entry with same path (move-to-top behavior)
        s_RecentProjects.erase(
            std::remove_if(s_RecentProjects.begin(), s_RecentProjects.end(),
                [&](const RecentProject& rp) { return rp.Path == absPath; }),
            s_RecentProjects.end());

        // Insert at front with current timestamp
        s_RecentProjects.insert(s_RecentProjects.begin(),
            { name, absPath, version, NowUnix() });

        // Cap at 10 entries
        if (s_RecentProjects.size() > 10)
            s_RecentProjects.resize(10);

        SaveRecentProjects();
    }

    void ProjectLauncher::SetPendingProject(const fs::path& luthprojPath)
    {
        s_PendingProject = luthprojPath;
        s_HasPending = true;
    }

    // ================================================================
    // Rendering
    // ================================================================

    void ProjectLauncher::Render()
    {
        // Center on the main application window
        const ImGuiViewport* viewport = ImGui::GetMainViewport();
        ImVec2 center = { viewport->Pos.x + viewport->Size.x * 0.5f,
                          viewport->Pos.y + viewport->Size.y * 0.5f };

        ImVec2 windowSize = { 620, 440 };
        ImGui::SetNextWindowPos(center, ImGuiCond_Always, { 0.5f, 0.5f });
        ImGui::SetNextWindowSize(windowSize, ImGuiCond_Always);

        ImGuiWindowFlags flags =
            ImGuiWindowFlags_NoCollapse |
            ImGuiWindowFlags_NoResize |
            ImGuiWindowFlags_NoDocking |
            ImGuiWindowFlags_NoMove |
            ImGuiWindowFlags_NoTitleBar;

        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, { 20, 16 });
        ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 6.0f);
        ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.10f, 0.10f, 0.10f, 1.0f));

        if (ImGui::Begin("##ProjectLauncher", nullptr, flags))
        {
            // ── Header: Logo + Title + Buttons ──
            float headerHeight = 40.0f;

            // Luth logo (blue square)
            ImDrawList* drawList = ImGui::GetWindowDrawList();
            ImVec2 logoPos = ImGui::GetCursorScreenPos();
            logoPos.y += 2;
            float logoSize = 32.0f;
            drawList->AddRectFilled(logoPos,
                { logoPos.x + logoSize, logoPos.y + logoSize },
                IM_COL32(0, 140, 255, 255), 4.0f);
            ImGui::Dummy({ logoSize, logoSize });

            // "Projects" title
            ImGui::SameLine(0, 12);

            ImGui::PushFont(Editor::GetMainFont());
            ImGui::SetWindowFontScale(1.2f);

            // Vertically center text relative to the 32px logo
            float textHeight = ImGui::GetFontSize();
            float textCenterY = logoPos.y + (logoSize - textHeight) * 0.5f;
            ImGui::SetCursorScreenPos({ ImGui::GetCursorScreenPos().x, textCenterY });

            ImGui::Text("Projects");

            ImGui::SetWindowFontScale(1.0f);
            ImGui::PopFont();

            // Buttons — right-aligned
            ImGui::SameLine();
            float buttonsWidth = 160.0f;
            ImGui::SetCursorPosX(windowSize.x - 20 - buttonsWidth);

            // Vertically center buttons relative to the 32px logo
            float buttonHeight = 26.0f;
            float buttonCenterY = logoPos.y + (logoSize - buttonHeight) * 0.5f;

            // Set Y pos for the first button
            ImGui::SetCursorScreenPos({ ImGui::GetCursorScreenPos().x, buttonCenterY });

            // "Add" button (open existing)
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.18f, 0.18f, 0.18f, 1.0f));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.25f, 0.25f, 0.25f, 1.0f));
            ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 4.0f);

            if (ImGui::Button(ICON_FA_FOLDER "  Add", { 72, buttonHeight }))
            {
                auto path = FileDialog::OpenFile("Luth Project (*.luthproj)\0*.luthproj\0All Files (*.*)\0*.*\0");
                if (path.has_value())
                {
                    s_PendingProject = path.value();
                    s_HasPending = true;
                    s_Visible = false;
                }
            }

            ImGui::SameLine(0, 6);

            // Explicitly set the exact Y position again for the second button
            // (SameLine can revert custom vertical offsets back to the line's default baseline)
            ImGui::SetCursorScreenPos({ ImGui::GetCursorScreenPos().x, buttonCenterY });

            // "+ New" button
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.0f, 0.0f, 0.0f, 0.0f));
            ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(0.0f, 0.55f, 0.85f, 1.0f));
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.0f, 0.65f, 1.0f, 1.0f));
            ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 1.2f);

            if (ImGui::Button("+ New", { 72, buttonHeight }))
            {
                s_ShowNewProjectDialog = true;
            }

            ImGui::PopStyleVar(1);   // FrameBorderSize
            ImGui::PopStyleColor(3); // + New colors
            ImGui::PopStyleVar(1);   // FrameRounding
            ImGui::PopStyleColor(2); // Add button colors

            ImGui::Dummy({ 0, 8 });

            // ── Project List ──
            DrawProjectList();
        }
        ImGui::End();
        ImGui::PopStyleColor(1); // WindowBg
        ImGui::PopStyleVar(2);   // Padding, Rounding

        // New project dialog
        if (s_ShowNewProjectDialog)
            DrawNewProjectDialog();
    }

    void ProjectLauncher::DrawProjectList()
    {
        float listHeight = ImGui::GetContentRegionAvail().y;

        ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.08f, 0.08f, 0.08f, 1.0f));
        ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 4.0f);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(4.0f, 4.0f));
        ImGui::BeginChild("##RecentList", { 0, listHeight }, ImGuiChildFlags_Border, ImGuiWindowFlags_AlwaysUseWindowPadding);

        if (s_RecentProjects.empty())
        {
            float cy = listHeight * 0.4f;
            ImGui::SetCursorPosY(cy);

            const char* msg1 = "No recent projects";
            float w1 = ImGui::CalcTextSize(msg1).x;
            ImGui::SetCursorPosX((ImGui::GetContentRegionAvail().x - w1) * 0.5f);
            ImGui::TextDisabled("%s", msg1);

            const char* msg2 = "Add an existing project or create a new one";
            float w2 = ImGui::CalcTextSize(msg2).x;
            ImGui::SetCursorPosX((ImGui::GetContentRegionAvail().x - w2) * 0.5f);
            ImGui::TextDisabled("%s", msg2);
        }
        else
        {
            float availWidth = ImGui::GetContentRegionAvail().x;

            for (int i = 0; i < (int)s_RecentProjects.size(); i++)
            {
                auto& rp = s_RecentProjects[i];
                ImGui::PushID(i);

                float rowHeight = ImGui::GetFontSize() * 2.6f;

                // Full-width selectable (double-click to open)
                if (ImGui::Selectable("##Row", false,
                    ImGuiSelectableFlags_AllowDoubleClick, { 0, rowHeight }))
                {
                    if (ImGui::IsMouseDoubleClicked(0))
                    {
                        s_PendingProject = rp.Path;
                        s_HasPending = true;
                        s_Visible = false;
                    }
                }

                // Context menu for removal
                if (ImGui::BeginPopupContextItem())
                {
                    if (ImGui::MenuItem(ICON_FA_XMARK "  Remove from list"))
                    {
                        s_RecentProjects.erase(s_RecentProjects.begin() + i);
                        SaveRecentProjects();
                        ImGui::EndPopup();
                        ImGui::PopID();
                        break;
                    }
                    ImGui::EndPopup();
                }

                ImVec2 rowMin = ImGui::GetItemRectMin();
                ImVec2 rowMax = ImGui::GetItemRectMax();
                float padX = 12.0f;

                // ── Row line 1: Name (left) ──
                float line1Y = rowMin.y + 5;
                
                // Name
                ImGui::SetCursorScreenPos({ rowMin.x + padX, line1Y });
                ImGui::Text("%s", rp.Name.c_str());

                // ── Vertically Centered Elements: Time (center-right) | Version (right) ──
                float centerY = rowMin.y + (rowHeight - ImGui::GetFontSize()) * 0.5f;

                // Time ago
                std::string timeStr = RelativeTimeString(rp.LastOpened);
                if (!timeStr.empty())
                {
                    float timeW = ImGui::CalcTextSize(timeStr.c_str()).x;
                    ImGui::SetCursorScreenPos({ rowMax.x - padX - 80 - timeW, centerY });
                    ImGui::TextDisabled("%s", timeStr.c_str());
                }

                // Version
                std::string verStr = "v" + rp.Version;
                float verW = ImGui::CalcTextSize(verStr.c_str()).x;
                ImGui::SetCursorScreenPos({ rowMax.x - padX - verW, centerY });
                ImGui::TextDisabled("%s", verStr.c_str());

                // ── Row line 2: Path (dimmed) ──
                float line2Y = line1Y + ImGui::GetFontSize() + 3;
                fs::path projectDir = fs::path(rp.Path).parent_path();
                std::string dirStr = projectDir.string();

                // Truncate long paths
                float maxPathW = availWidth - padX * 2 - 40;
                if (ImGui::CalcTextSize(dirStr.c_str()).x > maxPathW)
                {
                    // Show drive + ... + last folder
                    std::string root = projectDir.root_path().string();
                    std::string leaf = projectDir.filename().string();
                    dirStr = root + "..." + leaf;
                }

                ImGui::SetCursorScreenPos({ rowMin.x + padX, line2Y });
                ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.45f, 0.45f, 0.45f, 1.0f));
                ImGui::Text("%s", dirStr.c_str());
                ImGui::PopStyleColor();

                ImGui::PopID();
            }
        }

        ImGui::EndChild();
        ImGui::PopStyleVar(2);
        ImGui::PopStyleColor();
    }

    void ProjectLauncher::DrawNewProjectDialog()
    {
        ImGui::OpenPopup("New Project");

        ImVec2 dialogSize = { 400, 180 };
        ImGui::SetNextWindowSize(dialogSize, ImGuiCond_Always);
        const ImGuiViewport* vp = ImGui::GetMainViewport();
        ImVec2 center = { vp->Pos.x + vp->Size.x * 0.5f, vp->Pos.y + vp->Size.y * 0.5f };
        ImGui::SetNextWindowPos(center, ImGuiCond_Always, { 0.5f, 0.5f });

        if (ImGui::BeginPopupModal("New Project", &s_ShowNewProjectDialog, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoMove))
        {
            static char projectName[128] = "MyProject";
            static char projectDir[512] = "";

            if (projectDir[0] == '\0')
            {
                #ifdef _WIN32
                const char* userProfile = std::getenv("USERPROFILE");
                if (userProfile)
                    snprintf(projectDir, sizeof(projectDir), "%s\\Documents\\Luth Projects", userProfile);
                #endif
            }

            ImGui::Text("Project Name");
            ImGui::SetNextItemWidth(-1);
            ImGui::InputText("##ProjName", projectName, sizeof(projectName));

            ImGui::Spacing();
            ImGui::Text("Location");
            ImGui::SetNextItemWidth(-40);
            ImGui::InputText("##ProjDir", projectDir, sizeof(projectDir));
            ImGui::SameLine();
            if (ImGui::Button("..."))
            {
                // TODO: folder browser dialog
            }

            ImGui::Spacing();
            ImGui::Separator();
            ImGui::Spacing();

            float buttonWidth = 100.0f;
            ImGui::SetCursorPosX((dialogSize.x - buttonWidth * 2 - ImGui::GetStyle().ItemSpacing.x) * 0.5f);

            if (ImGui::Button("Create", { buttonWidth, 0 }) && projectName[0] != '\0' && projectDir[0] != '\0')
            {
                fs::path dir = fs::path(projectDir) / projectName;
                fs::path projFile = dir / (std::string(projectName) + ".luthproj");

                fs::create_directories(dir / "assets" / "models");
                fs::create_directories(dir / "assets" / "textures");
                fs::create_directories(dir / "assets" / "scenes");
                fs::create_directories(dir / "assets" / "materials");

                ProjectFile pf;
                pf.Name = projectName;
                pf.Save(projFile);

                s_PendingProject = projFile;
                s_HasPending = true;
                s_Visible = false;
                s_ShowNewProjectDialog = false;
                ImGui::CloseCurrentPopup();
            }

            ImGui::SameLine();
            if (ImGui::Button("Cancel", { buttonWidth, 0 }))
            {
                s_ShowNewProjectDialog = false;
                ImGui::CloseCurrentPopup();
            }

            ImGui::EndPopup();
        }
    }
}
