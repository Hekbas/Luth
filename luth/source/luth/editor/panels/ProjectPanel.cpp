#include "luthpch.h"
#include "luth/editor/panels/ProjectPanel.h"
#include "luth/editor/panels/InspectorPanel.h"
#include "luth/resources/resourceDB.h"
#include "luth/utils/LuthIcons.h"

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
        node.Uuid = ResourceDB::PathToUuid(path);
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
                        fileNode.Uuid = ResourceDB::PathToUuid(entry.path());
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
        const fs::path currentPath = ResourceDB::UuidToPath(m_CurrentDirectoryUuid);
        const fs::path rootPath = ResourceDB::UuidToPath(m_RootNode.Uuid);
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
                m_CurrentDirectoryUuid = ResourceDB::PathToUuid(accumulatedPath);
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

        // Handle right-click context menu
        if (ImGui::BeginPopupContextWindow("ProjectContextMenu")) {
            DrawCreateMenu();
            ImGui::EndPopup();
        }

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
            ImGui::PushFont(Editor::GetIconFont());
            ImGui::PushStyleColor(ImGuiCol_Text, { 0.5, 0.5, 0.5, 1.0 });
            ImGui::Button(child.Directories.empty() && child.Contents.empty() ?
                ICON_FOLDER_E : ICON_FOLDER, ImVec2(thumbnailSize, thumbnailSize));
            ImGui::PopStyleColor();
            ImGui::PopFont();

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
            const char* icon;
            ImVec4 iconColor = ImVec4(1.0f, 1.0f, 1.0f, 1.0f);
            switch (child.Type) {
                case ResourceType::Model:    icon = ICON_MODEL;    break;
                case ResourceType::Texture:  icon = ICON_TEXTURE;  break;
                case ResourceType::Material: icon = ICON_MATERIAL; break;
                case ResourceType::Shader:   icon = ICON_FILE;     break;
                case ResourceType::Font:     icon = ICON_FILE;     break;
                case ResourceType::Config:   icon = ICON_FILE;     break;
                case ResourceType::Unknown:  icon = ICON_FILE;     break;
            }
            ImGui::PushFont(Editor::GetIconFont());
            Vec4 color = FileSystem::GetTypeInfo().at(child.Type).color;
            ImGui::PushStyleColor(ImGuiCol_Text, { color.r, color.g, color.b, color.a });
            ImGui::Button(icon, ImVec2(thumbnailSize, thumbnailSize));
            ImGui::PopStyleColor();
            ImGui::PopFont();

            // Drag handling (priority)
            if (child.Type == ResourceType::Model || child.Type == ResourceType::Material || child.Type == ResourceType::Texture) {
                if (ImGui::BeginDragDropSource(ImGuiDragDropFlags_SourceAllowNullID)) {
                    ImGui::SetDragDropPayload("ASSET_UUID", &child.Uuid, sizeof(Luth::UUID));
                    ImGui::Text("%s", child.Name.c_str());
                    ImGui::EndDragDropSource();
                }
            }

            // Click handling (only if pressed AND released on the same item)
            if (ImGui::IsItemDeactivated() && ImGui::IsMouseReleased(0) && ImGui::IsItemHovered()) {
                if (auto* inspector = Editor::GetPanel<InspectorPanel>()) {
                    inspector->SetSelectedResource(child.Uuid);
                }
            }

            // Double-click handling
            if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(0)) {
                // Handle file double-click
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

    void ProjectPanel::DrawCreateMenu()
    {
        if (ImGui::BeginMenu("Create")) {
            if (ImGui::MenuItem("Material")) {
                CreateNewMaterial();
            }
            // TODO: Add other create options here...
            ImGui::EndMenu();
        }
    }

    void ProjectPanel::CreateNewMaterial()
    {
        // Current directory
        fs::path currDir = ResourceDB::UuidToPath(m_CurrentDirectoryUuid);

        // Default material path
        fs::path newMaterialPath = currDir / "NewMaterial.mat";

        // Ensure unique filename
        int counter = 1;
        while (fs::exists(newMaterialPath)) {
            newMaterialPath = currDir /
                ("NewMaterial_" + std::to_string(counter++) + ".mat");
        }

        // Create the material file
        std::ofstream file(newMaterialPath);
        if (!file.is_open()) {
            LH_CORE_ERROR("Failed to create material file: {0}", newMaterialPath.string());
            return;
        }

        // Create default material content
        nlohmann::json materialData;
        materialData["shader"] = ""; // Empty shader by default
        materialData["textures"] = nlohmann::json::object();
        file << materialData.dump(4);
        file.close();

        // Create .meta file
        fs::path metaPath = newMaterialPath;
        metaPath += ".meta";

        UUID uuid;
        MetaFile meta(uuid); // New random UUID
        meta.Save(metaPath);

        // Register with resource database
        ResourceDB::RegisterAsset(newMaterialPath, meta.GetUUID());

        // Notify listeners
        /*if (OnMaterialCreated) {
            OnMaterialCreated(newMaterialPath);
        }*/

        LH_CORE_INFO("Created new material: {0}", newMaterialPath.string());
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
