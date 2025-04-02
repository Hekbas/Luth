#include "luthpch.h"
#include "luth/editor/panels/ProjectPanel.h"
#include "luth/resources/resourceDB.h"

namespace Luth
{
    ProjectPanel::ProjectPanel()
    {
        LH_CORE_INFO("Created Project panel");
    }

    void ProjectPanel::OnInit()
    {
        m_AssetsPath = FileSystem::AssetsPath().string();

        m_RootNode = BuildDirectoryTree(m_AssetsPath);
        m_CurrentDirectoryUuid = m_RootNode.Uuid;;
    }

    void ProjectPanel::OnRender()
    {
        if (ImGui::Begin("Project"))
        {
            // Left panel - directory tree
            ImGui::BeginChild("##ProjectTree", ImVec2(ImGui::GetWindowWidth() * 0.2f, 0), ImGuiChildFlags_ResizeX);
            ImGui::SetNextItemOpen(true);
            DrawDirectoryNode(m_RootNode);
            ImGui::EndChild();

            ImGui::SameLine();

            // Right panel - Split view
            ImGui::BeginChild("##ProjectSplitView", ImVec2(0, 0), true, ImGuiWindowFlags_HorizontalScrollbar);

            // Top bar with path
            DrawPathBar();

            // Directory contents
            ImGui::BeginChild("##ProjectContent", ImVec2(0, 0), true);
            DrawDirectoryContent();
            ImGui::EndChild();

            ImGui::EndChild();
        }
        ImGui::End();
    }

    DirectoryNode ProjectPanel::BuildDirectoryTree(const fs::path& path)
    {
        DirectoryNode node;
        node.Uuid = ResourceDB::GetUuidForPath(path);
        node.Name = path.filename().string();
        node.Type = ResourceType::Directory;

        if (node.Name.empty())
            node.Name = "Assets";

        try {
            std::vector<fs::directory_entry> entries;

            // Collect all entries
            for (const auto& entry : fs::directory_iterator(path)) {
                entries.push_back(entry);
            }

            // Process directories first
            for (const auto& entry : entries) {
                if (entry.is_directory()) {
                    DirectoryNode child = BuildDirectoryTree(entry.path());
                    node.Directories.push_back(child);
                }
            }

            // Then process files
            for (const auto& entry : entries) {
                if (!entry.is_directory()) {
                    ResourceType fileType = FileSystem::ClassifyFileType(entry.path());
                    if (fileType != ResourceType::Unknown) {
                        DirectoryNode fileNode;
                        fileNode.Uuid = ResourceDB::GetUuidForPath(entry.path());
                        fileNode.Name = entry.path().filename().string();
                        fileNode.Type = fileType;
                        node.Contents.push_back(fileNode);
                    }
                }
            }
        }
        catch (...) {
            // TODO: Handle directory errors
        }

        return node;
    }

    void ProjectPanel::DrawDirectoryNode(DirectoryNode& node)
    {
        ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_OpenOnArrow | ImGuiTreeNodeFlags_OpenOnDoubleClick;
        if (node.Directories.empty()) flags |= ImGuiTreeNodeFlags_Leaf;
        if (node.Uuid == m_CurrentDirectoryUuid) flags |= ImGuiTreeNodeFlags_Selected;

        bool isOpen = ImGui::TreeNodeEx(node.Name.c_str(), flags);

        if (ImGui::IsItemClicked()) {
            m_CurrentDirectoryUuid = node.Uuid;
        }

        if (isOpen) {
            for (auto& child : node.Directories) {
                DrawDirectoryNode(child);
            }
            ImGui::TreePop();
        }
    }

    void ProjectPanel::DrawPathBar()
    {
        const fs::path currentPath = ResourceDB::ResolveUuid(m_CurrentDirectoryUuid);
        const fs::path rootPath = ResourceDB::ResolveUuid(m_RootNode.Uuid);
        const fs::path relativePath = fs::relative(currentPath, rootPath);

        ImGui::BeginChild("##PathBar", ImVec2(0, ImGui::GetFontSize() + ImGui::GetStyle().FramePadding.y * 2), false);

        // Start building from the root
        fs::path accumulatedPath = rootPath;

        if (ImGui::Button("Assets")) {
            m_CurrentDirectoryUuid = m_RootNode.Uuid;
        }

        for (const auto& part : relativePath) {
            if (part.empty() || part == ".") continue;

            ImGui::SameLine();
            ImGui::Text(">");
            ImGui::SameLine();

            // Store the path before appending the part
            const fs::path previousPath = accumulatedPath;
            accumulatedPath /= part;

            if (ImGui::Button(part.string().c_str())) {
                m_CurrentDirectoryUuid = ResourceDB::GetUuidForPath(accumulatedPath);
                break;
            }
        }

        ImGui::EndChild();
    }

    void ProjectPanel::DrawDirectoryContent()
    {
        // Find current directory node
        const DirectoryNode* currentDir = FindNodeByUuid(m_RootNode, m_CurrentDirectoryUuid);
        if (!currentDir) return;

        // Grid layout settings
        const float padding = 16.0f;
        const float thumbnailSize = 64.0f;
        const float cellSize = thumbnailSize + padding;
        const float panelWidth = ImGui::GetContentRegionAvail().x;
        const int columnCount = std::max(1, (int)(panelWidth / cellSize));

        ImGui::Columns(columnCount, 0, false);

        // Display subdirectories
        for (const auto& child : currentDir->Directories) {
            ImGui::PushID(child.Uuid.ToString().c_str());

            // Icon button
            ImGui::BeginGroup();
            /*ImGui::Button(child.Type == ResourceType::Directory ?
                ICON_FA_FOLDER "##dir" : ICON_FA_FILE "##file",
                ImVec2(thumbnailSize, thumbnailSize));*/

            ImGui::Button("##file", ImVec2(thumbnailSize, thumbnailSize));

            // Double-click handling
            if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(0)) {
                m_CurrentDirectoryUuid = child.Uuid;
            }

            // Name label
            ImGui::TextWrapped("%s", child.Name.c_str());
            ImGui::EndGroup();

            // Context menu
            //if (ImGui::BeginPopupContextItem()) {
            //    if (ImGui::MenuItem("Rename")) { /* ... */ }
            //    if (ImGui::MenuItem("Delete")) { /* ... */ }
            //    ImGui::EndPopup();
            //}

            ImGui::NextColumn();
            ImGui::PopID();
        }

        // Display directory contents
        for (const auto& child : currentDir->Contents) {
            ImGui::PushID(child.Uuid.ToString().c_str());

            // Icon button
            ImGui::BeginGroup();
            /*ImGui::Button(child.Type == ResourceType::Directory ?
                ICON_FA_FOLDER "##dir" : ICON_FA_FILE "##file",
                ImVec2(thumbnailSize, thumbnailSize));*/

            ImGui::Button("##file", ImVec2(thumbnailSize, thumbnailSize));

            // Double-click handling
            if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(0)) {
                if (child.Type == ResourceType::Directory) {
                    m_CurrentDirectoryUuid = child.Uuid;
                }
                else {
                    // Handle file double-click
                }
            }

            // Drag handling
            if (child.Type == ResourceType::Model) {
                if (ImGui::BeginDragDropSource(ImGuiDragDropFlags_SourceAllowNullID)) {
                    ImGui::SetDragDropPayload("ASSET_UUID", &child.Uuid, sizeof(Luth::UUID));
                    ImGui::Text("%s", child.Name.c_str()); // Preview during drag
                    ImGui::EndDragDropSource();
                }
            }

            // Name label
            ImGui::TextWrapped("%s", child.Name.c_str());
            ImGui::EndGroup();

            // Context menu
            //if (ImGui::BeginPopupContextItem()) {
            //    if (ImGui::MenuItem("Rename")) { /* ... */ }
            //    if (ImGui::MenuItem("Delete")) { /* ... */ }
            //    ImGui::EndPopup();
            //}

            ImGui::NextColumn();
            ImGui::PopID();
        }

        ImGui::Columns(1);
    }

    const DirectoryNode* ProjectPanel::FindNodeByUuid(const DirectoryNode& node, const UUID& uuid)
    {
        if (node.Uuid == uuid) return &node;

        for (const auto& child : node.Directories) {
            if (const auto* found = FindNodeByUuid(child, uuid)) {
                return found;
            }
        }
        return nullptr;
    }
}
