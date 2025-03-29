#include "luthpch.h"
#include "luth/editor/panels/ProjectPanel.h"

namespace Luth
{
    ProjectPanel::ProjectPanel() {}

    void ProjectPanel::OnInit()
    {
        m_AssetsPath = ResourceManager::GetBasePath().string();
        m_CurrentDirectory = m_AssetsPath;
    }

    void ProjectPanel::OnRender()
    {
        if (ImGui::Begin("Project"))
        {
            // Left panel - directory tree
            ImGui::BeginChild("##ProjectTree", ImVec2(ImGui::GetWindowWidth() * 0.2f, 0), ImGuiChildFlags_ResizeX);
            DrawDirectoryTree(m_AssetsPath);
            ImGui::EndChild();

            ImGui::SameLine();

            // Right panel - Split view
            ImGui::BeginChild("##ProjectSplitView", ImVec2(0, 0), true, ImGuiWindowFlags_HorizontalScrollbar);

            // Top bar with path
            DrawPathBar();

            // Directory contents
            ImGui::BeginChild("##ProjectContent", ImVec2(0, 0), true);
            DrawDirectoryContents();
            ImGui::EndChild();

            ImGui::EndChild();
        }
        ImGui::End();
    }

    void ProjectPanel::DrawPathBar()
    {
        ImGui::BeginChild("##PathBar", ImVec2(0, ImGui::GetFontSize() + ImGui::GetStyle().FramePadding.y * 2), false);

        fs::path relativePath = fs::relative(m_CurrentDirectory, m_AssetsPath);
        fs::path accumulatedPath = m_AssetsPath;

        if (ImGui::Button("Assets")) {
            m_CurrentDirectory = m_AssetsPath;
        }

        // Iterate through each component of the relative path
        for (const auto& part : relativePath) {
            if (part.empty() || part == ".") continue;

            ImGui::SameLine();
            ImGui::Text(">");
            ImGui::SameLine();

            accumulatedPath /= part;

            if (ImGui::Button(part.string().c_str())) {
                m_CurrentDirectory = accumulatedPath.string();
                break;
            }
        }

        ImGui::EndChild();
    }

    void ProjectPanel::DrawDirectoryTree(const std::string& path)
    {
        fs::path rootPath(path);
        DrawDirectoryNode(rootPath, true);
    }

    void ProjectPanel::DrawDirectoryNode(const fs::path& path, bool isRoot)
    {
        ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_OpenOnArrow | ImGuiTreeNodeFlags_OpenOnDoubleClick;
        if (!fs::exists(path)) return;

        bool isDirectory = fs::is_directory(path);
        bool isSelected = (fs::equivalent(path, m_CurrentDirectory));

        if (isSelected) {
            flags |= ImGuiTreeNodeFlags_Selected;
        }

        if (!isDirectory) return;

        std::string name = path.filename().string();
        if (name.empty() && isRoot) {
            name = "Assets";
        }

        bool hasChildren = false;
        for (const auto& entry : fs::directory_iterator(path)) {
            if (fs::is_directory(entry.path())) {
                hasChildren = true;
                break;
            }
        }

        if (!hasChildren) {
            flags |= ImGuiTreeNodeFlags_Leaf;
        }

        bool isOpen = ImGui::TreeNodeEx(name.c_str(), flags);

        if (ImGui::IsItemClicked() && !ImGui::IsItemToggledOpen()) {
            m_CurrentDirectory = path.string();
        }

        if (isOpen) {
            // Sort directories alphabetically
            std::vector<fs::path> directories;
            for (const auto& entry : fs::directory_iterator(path)) {
                if (fs::is_directory(entry.path())) {
                    directories.push_back(entry.path());
                }
            }
            std::sort(directories.begin(), directories.end());

            for (const auto& dir : directories) {
                DrawDirectoryNode(dir);
            }

            ImGui::TreePop();
        }
    }

    void ProjectPanel::DrawDirectoryContents()
    {
        static float padding = 16.0f;
        static float thumbnailSize = 64.0f;
        float cellSize = thumbnailSize + padding;

        float panelWidth = ImGui::GetContentRegionAvail().x;
        int columnCount = (int)(panelWidth / cellSize);
        if (columnCount < 1) columnCount = 1;

        ImGui::Columns(columnCount, 0, false);

        for (const auto& entry : fs::directory_iterator(m_CurrentDirectory)) {
            const auto& path = entry.path();
            std::string filename = path.filename().string();

            ImGui::PushID(filename.c_str());

            ImGui::BeginGroup();

            // Draw icon (placeholder)
            ImGui::Button("", ImVec2(thumbnailSize, thumbnailSize));

            // Draw filename
            ImGui::TextWrapped("%s", filename.c_str());

            ImGui::EndGroup();

            if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(0)) {
                if (fs::is_directory(path)) {
                    m_CurrentDirectory = path.string();
                }
                // else: handle file opening
            }

            ImGui::NextColumn();
            ImGui::PopID();
        }

        ImGui::Columns(1);
    }
}
