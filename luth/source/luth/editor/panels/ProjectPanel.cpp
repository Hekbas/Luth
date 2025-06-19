#include "luthpch.h"
#include "luth/editor/panels/ProjectPanel.h"
#include "luth/editor/panels/InspectorPanel.h"
#include "luth/renderer/RendererAPI.h"
#include "luth/resources/resourceDB.h"
#include "luth/resources/libraries/ModelLibrary.h"
#include "luth/resources/libraries/MaterialLibrary.h"
#include "luth/utils/ImGuiUtils.h"
#include "luth/utils/LuthIcons.h"

namespace Luth
{
    ProjectPanel::ProjectPanel()
    {
        LH_CORE_INFO("Created Project panel");
    }

    void ProjectPanel::OnInit()
    {
        m_InspectorPanel = Editor::GetPanel<InspectorPanel>();
        m_AssetsPath = FileSystem::AssetsPath().string();

        m_RootNode = BuildDirectoryTree(m_AssetsPath);
        m_CurrentDirectory = m_RootNode;
    }

    void ProjectPanel::OnRender()
    {
        ImGui::PushFont(Editor::GetFASolid());
        std::string project = ICON_FA_FOLDER + std::string("  Project");

        if (ImGui::Begin(project.c_str()))
        {
            // Left panel - directory tree
            ImGui::BeginChild("##ProjectTree", ImVec2(ImGui::GetWindowWidth() * 0.2f, 0), ImGuiChildFlags_ResizeX);
            //ImGui::Dummy({ 0, 4 });
            ImGui::SetNextItemOpen(true);
            DrawDirectoryNode(*m_RootNode);
            ImGui::EndChild();

            ImGui::SameLine();

            // Right panel - Split view
            ImGui::PushStyleColor(ImGuiCol_ChildBg, IM_COL32(0, 0, 0, 0));
            ImGui::BeginChild("##ProjectSplitView", ImVec2(0, 0), true, ImGuiWindowFlags_HorizontalScrollbar);
            ImGui::PopStyleColor();

            // Top bar with path
            DrawPathBar();
            ImGui::SameLine();
			if (ImGui::Button("#")) m_ListView = !m_ListView;

            // Directory contents
            ImGui::BeginChild("##ProjectContent", ImVec2(0, 0), true);
            DrawDirectoryContent();
            ImGui::EndChild();

            ImGui::EndChild();
        }
        ImGui::End();
        ImGui::PopFont();

        if (m_NodeToDelete) ShowDeleteConfirmation();
    }

    DirectoryNode* ProjectPanel::BuildDirectoryTree(const fs::path& path, DirectoryNode* parent)
    {
        DirectoryNode* node = new DirectoryNode();
        node->Uuid = ResourceDB::PathToUuid(path);
        node->Name = path.filename().string();
        node->Type = ResourceType::Directory;
        node->Parent = parent;

        if (node->Name.empty()) {
            node->Name = "Assets";
            node->IsOpen = true;
        }

        try {
            std::vector<fs::directory_entry> entries;
            for (const auto& entry : fs::directory_iterator(path)) {
                entries.push_back(entry);
            }

            // Process directories
            for (const auto& entry : entries) {
                if (entry.is_directory()) {
                    auto child = BuildDirectoryTree(entry.path(), node);
                    node->Directories.push_back(std::move(child));
                }
            }

            // Process files
            for (const auto& entry : entries) {
                if (!entry.is_directory()) {
                    ResourceType fileType = FileSystem::ClassifyFileType(entry.path());
                    if (fileType != ResourceType::Unknown) {
						DirectoryNode* fileNode = new DirectoryNode();
                        fileNode->Uuid = ResourceDB::PathToUuid(entry.path());
                        fileNode->Name = entry.path().filename().stem().string();
                        fileNode->Type = fileType;
                        fileNode->Parent = node;
                        node->Contents.push_back(std::move(fileNode));
                    }
                }
            }
        }
        catch (...) {
            // Handle errors
        }

        return node;
    }

    DirectoryNode* ProjectPanel::FindNode(DirectoryNode& root, const DirectoryNode& target)
    {
        if (&root == &target) return &root;

        // Search Directories
        for (auto& dir : root.Directories) {
            if (dir == &target) return dir;
            DirectoryNode* found = FindNode(*dir, target);
            if (found) return found;
        }

        // Search Contents
        for (auto& content : root.Contents) {
            if (content == &target) return content;
        }

        return nullptr;
    }

    // TODO: I dont quite like this beeing DFS :/
    bool ProjectPanel::DeleteNode(DirectoryNode& root, const DirectoryNode& target)
    {
        // Check Directories
        for (auto it = root.Directories.begin(); it != root.Directories.end(); ++it) {
            if (*it == &target) {
                delete* it;
                root.Directories.erase(it);
                return true;
            }
            if (DeleteNode(**it, target)) {
                return true;
            }
        }

        // Check Contents
        for (auto it = root.Contents.begin(); it != root.Contents.end(); ++it) {
            if (*it == &target) {
                delete* it;
                root.Contents.erase(it);
                return true;
            }
        }

        return false;
    }

    void ProjectPanel::DrawDirectoryNode(DirectoryNode& node)
    {
        // Setup flags
        ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_SpanFullWidth |
            ImGuiTreeNodeFlags_OpenOnArrow | ImGuiTreeNodeFlags_OpenOnDoubleClick;
        if (node.Directories.empty()) flags |= ImGuiTreeNodeFlags_Leaf;
        if (&node == m_CurrentDirectory) flags |= ImGuiTreeNodeFlags_Selected;
        if (node.Name == "Assets") flags |= ImGuiTreeNodeFlags_Framed;

        //if (node.IsOpen) ImGui::SetNextItemOpen(true);

        // Set Icon
        const char* icon = ICON_FA_FOLDER;
        if (node.Directories.empty() && node.Contents.empty()) {
            ImGui::PushFont(Editor::GetFARegular());
        }
        else if (node.IsOpen && !node.Directories.empty()) {
            icon = ICON_FA_FOLDER_OPEN;
            ImGui::PushFont(Editor::GetFARegular());
		}
		else {
			ImGui::PushFont(Editor::GetFASolid());
        }
            
        // Draw the node
        node.IsOpen = ImGui::TreeNodeEx((void*)&node, flags, "%s", icon);
        ImGui::PopFont();

        if (ImGui::IsItemClicked()) {
            m_CurrentDirectory = &node;
        }

        ImGui::SameLine();
        ImGui::Text(node.Name.c_str());
        
        // Visual line settings
        const ImColor treeLineColor = ImColor(128, 128, 128, 128);
        const float smallOffsetX = -6.0f;
        ImVec2 verticalLineStart = ImGui::GetCursorScreenPos();
        ImDrawList* drawList = ImGui::GetWindowDrawList();

        if (node.IsOpen) {
            verticalLineStart.x += smallOffsetX; // My ocd will kill me
            ImVec2 verticalLineEnd = verticalLineStart;

            for (auto& child : node.Directories) {
                auto currentPos = ImGui::GetCursorScreenPos();

                // Calculate horizontal line size
                float horizontalTreeLineSize = 20.0f;
                if (!child->Directories.empty())
                    horizontalTreeLineSize *= 0.5f;

                // Draw child node
                DrawDirectoryNode(*child);

                // Draw horizontal line
                const ImRect childRect = ImRect(currentPos, currentPos + ImVec2(0.0f, ImGui::GetFontSize()));
                const float midpoint = (childRect.Min.y + childRect.Max.y) * 0.5f;
                drawList->AddLine(
                    ImVec2(verticalLineStart.x, midpoint),
                    ImVec2(verticalLineStart.x + horizontalTreeLineSize, midpoint),
                    treeLineColor);

                verticalLineEnd.y = midpoint;
            }

            // Draw vertical line
            drawList->AddLine(verticalLineStart, verticalLineEnd, treeLineColor);

            ImGui::TreePop();
        }
    }

    void ProjectPanel::DrawPathBar()
    {
        if (!m_CurrentDirectory) return;

        ImGui::BeginChild("##PathBar", ImVec2(-26, ImGui::GetFontSize() + ImGui::GetStyle().FramePadding.y * 2), false);

        // Store path segments in reverse order (from current to root)
        std::vector<DirectoryNode*> pathSegments;
        for (DirectoryNode* node = m_CurrentDirectory; node != nullptr; node = node->Parent) {
            pathSegments.push_back(node);
        }

        // Reverse to get root-to-current order
        std::reverse(pathSegments.begin(), pathSegments.end());

        // Draw the path segments
        bool isFirst = true;
        for (auto* segment : pathSegments) {
            if (!isFirst) {
                ImGui::SameLine();
                ImGui::Text(">");
                ImGui::SameLine();
            }

            // Special styling for root (Assets)
            if (segment->Name == "Assets") {
                if (ImGui::Button("Assets", ImVec2(0, 0))) {
                    m_CurrentDirectory = segment;
                }
            }
            else {
                // Calculate text size for proper alignment
                const ImVec2 textSize = ImGui::CalcTextSize(segment->Name.c_str());

                // Use Selectable for clickable segments with proper sizing
                if (ImGui::Selectable(segment->Name.c_str(), false, 0, textSize)) {
                    m_CurrentDirectory = segment;
                }
            }

            isFirst = false;
        }

        ImGui::EndChild();
    }

    void ProjectPanel::DrawDirectoryContent()
    {
        if (!m_CurrentDirectory) return;

        if (ImGui::BeginPopupContextWindow("ProjectContextMenu")) {
            DrawCreateMenu();
            ImGui::EndPopup();
        }

        m_ListView ? DrawListView() : DrawGridView();
    }

    void ProjectPanel::DrawListView()
    {
        ImGui::Dummy({ 0, 2 });
        DrawListItems(m_CurrentDirectory->Directories, true);
        DrawListItems(m_CurrentDirectory->Contents, false);
    }

    void ProjectPanel::DrawListItems(std::vector<DirectoryNode*>& items, bool isDirectory)
    {
        ImGui::Indent(8.0f);
        for (auto& item : items) {
            ImGui::PushID((void*)item);
            DrawListItem(*item, isDirectory);
            ImGui::PopID();
        }
        ImGui::Unindent(8.0f);
    }

    void ProjectPanel::DrawListItem(DirectoryNode& item, bool isDirectory)
    {
        // Set Icon
        const char* icon = ICON_FA_FOLDER;
        if (isDirectory) {
            if (item.Directories.empty() && item.Contents.empty()) {
                ImGui::PushFont(Editor::GetFARegular());
            }
            else {
                ImGui::PushFont(Editor::GetFASolid());
            }
        }
        else {
			icon = GetResourceIcon(item.Type);
            Vec4 color = FileSystem::GetTypeInfo().at(item.Type).color;
            ImGui::PushStyleColor(ImGuiCol_Text, { color.r, color.g, color.b, color.a });
            ImGui::PushFont(Editor::GetFASolid());
        }

        // Draw Icon
        ImGui::Text(icon);
        if (!isDirectory) ImGui::PopStyleColor();
        ImGui::PopFont();

        // Name with same-line alignment
        ImGui::SameLine();

        const bool isRenaming = (m_NodeToRename == &item);
        if (isRenaming) {
            HandleRenaming();
        }
        else {
            const bool isSelected = (m_SelectedNode == &item);
            ImGuiSelectableFlags flags = ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowDoubleClick;

            if (ImGui::Selectable(item.Name.c_str(), isSelected, flags)) {
                HandleItemInteraction(item, isDirectory);
            }

            // Drag and drop support
            if (!isDirectory && (item.Type == ResourceType::Model ||
                item.Type == ResourceType::Material ||
                item.Type == ResourceType::Texture))
            {
                HandleDragDrop(item);
            }

            // Right-click
            if (ImGui::IsItemHovered() && ImGui::IsMouseClicked(1)) {
                m_NodeMenu = &item;
            }
        }
    }

    void ProjectPanel::DrawGridView()
    {
        const float thumbnailSize = 64.0f;
        const float padding = 16.0f;
        const float cellSize = thumbnailSize + padding;
        const int columnCount = (int)(ImGui::GetContentRegionAvail().x / cellSize);

        ImGui::Columns(std::max(1, columnCount), 0, false);
        DrawGridItems(m_CurrentDirectory->Directories, true);
        DrawGridItems(m_CurrentDirectory->Contents, false);
        ImGui::Columns(1);
    }

    void ProjectPanel::DrawGridItems(std::vector<DirectoryNode*>& items, bool isDirectory)
    {
        for (auto& item : items) {
            ImGui::PushID((void*)item);
            DrawGridItem(*item, isDirectory);
            ImGui::NextColumn();
            ImGui::PopID();
        }
    }

    void ProjectPanel::DrawGridItem(DirectoryNode& item, bool isDirectory)
    {
        const float thumbnailSize = 64.0f;

        // Icon Button
        ImGui::BeginGroup();
        {
            // Set Icon
            const char* icon = ICON_FA_FOLDER;
            if (isDirectory) {
                if (item.Directories.empty() && item.Contents.empty()) {
                    ImGui::PushFont(Editor::GetFARegular());
                }
                else {
                    ImGui::PushFont(Editor::GetFASolid());
                }
            }
            else {
                icon = GetResourceIcon(item.Type);
                Vec4 color = FileSystem::GetTypeInfo().at(item.Type).color;
                ImGui::PushStyleColor(ImGuiCol_Text, { color.r, color.g, color.b, color.a });
                ImGui::PushFont(Editor::GetFASolid());
            }

            ImGui::SetWindowFontScale(3.0f);
            ImGui::Button(icon, { thumbnailSize, thumbnailSize });
            ImGui::SetWindowFontScale(1.0f);

            if (!isDirectory) ImGui::PopStyleColor();
            ImGui::PopFont();

            // Handle interactions
            if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(0)) {
                if (isDirectory) m_CurrentDirectory = &item;
            }

            // Name label
            ImGui::TextWrapped("%s", item.Name.c_str());
        }
        ImGui::EndGroup();

        // Drag and drop support
        if (!isDirectory && (item.Type == ResourceType::Model ||
            item.Type == ResourceType::Material ||
            item.Type == ResourceType::Texture))
        {
            HandleDragDrop(item);
        }
    }

    const char* ProjectPanel::GetResourceIcon(ResourceType type)
    {
        static const std::unordered_map<ResourceType, const char*> icons = {
            { ResourceType::Model,    ICON_FA_CUBE                  },
            { ResourceType::Texture,  ICON_FA_IMAGE                 },
            { ResourceType::Material, ICON_FA_CIRCLE_HALF_STROKE    },
			{ ResourceType::Shader,   ICON_FA_FILE_CODE             },
			{ ResourceType::Font,     ICON_FA_FONT                  },
			{ ResourceType::Config,   ICON_FA_FILE_LINES            },
			{ ResourceType::Unknown,  ICON_FA_FILE_CIRCLE_QUESTION  }
        };
        return icons.count(type) ? icons.at(type) : ICON_FILE;
    }

    void ProjectPanel::HandleDragDrop(DirectoryNode& item)
    {
        if (ImGui::BeginDragDropSource(ImGuiDragDropFlags_SourceAllowNullID)) {
            ImGui::SetDragDropPayload("ASSET_UUID", &item.Uuid, sizeof(Luth::UUID));
            ImGui::Text("%s", item.Name.c_str());
            ImGui::EndDragDropSource();
        }
    }

    void ProjectPanel::HandleItemInteraction(DirectoryNode& item, bool isDirectory)
    {
        if (ImGui::IsMouseDoubleClicked(0)) {
            if (isDirectory) {
                m_CurrentDirectory->IsOpen = true;
                m_CurrentDirectory = &item;
			}
            else {
                m_SelectedNode = &item;
                m_InspectorPanel->SetSelectedResource(item.Uuid);
            }
        }
        else {  // single click
            if (isDirectory) {

            }
            else {
                if (m_SelectedNode == &item) {
                }
                else {
                    m_SelectedNode = &item;
                    m_InspectorPanel->SetSelectedResource(item.Uuid);
                }
            }
        }
    }

    void ProjectPanel::HandleRenaming()
    {
        if (!m_NodeToRename) return;

        // Setup input text flags and focus
        constexpr ImGuiInputTextFlags flags = ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_AutoSelectAll;
        ImGui::SetKeyboardFocusHere();
        ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x);

        // Text Input
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(0, 0));
        const bool finish = ImGui::InputText("##Rename", m_RenameBuffer, sizeof(m_RenameBuffer), flags);
        ImGui::PopStyleVar();

        // Check for cancellation (Escape key)
        const bool cancel = ImGui::IsItemActive() && ImGui::IsKeyPressed(ImGuiKey_Escape);

        if (!finish && !cancel) return;

        if (finish && !cancel) {
            // Validate and apply new name
            std::string newName(m_RenameBuffer);
            if (newName.empty()) newName = m_OriginalName;
            m_NodeToRename->Name = newName;
			RenameResource(*m_NodeToRename, newName);
        }
        else if (cancel) {
            // Restore original name
            m_NodeToRename->Name = m_OriginalName;
        }

        // Cleanup
        m_NodeToRename = nullptr;
        m_OriginalName.clear();
        memset(m_RenameBuffer, 0, sizeof(m_RenameBuffer));
    }

    void ProjectPanel::DrawCreateMenu()
    {
        if (ImGui::BeginMenu("Create")) {
            if (ImGui::MenuItem("Folder")) {
                CreateNewFolder();
            }
            if (ImGui::MenuItem("Material")) {
                CreateNewMaterial();
            }
            // TODO: Add other create options here...
            ImGui::EndMenu();
        }

        if (m_NodeMenu) {
            ImGui::Separator();

            if (ImGui::MenuItem("Rename")) {
				m_NodeToRename = m_NodeMenu;
                strncpy_s(m_RenameBuffer, m_NodeToRename->Name.c_str(), sizeof(m_RenameBuffer));
				m_NodeMenu = nullptr;
            }
            if (ImGui::MenuItem("Delete")) {
				m_NodeToDelete = m_NodeMenu;
                ImGui::OpenPopup("Delete?");
                m_NodeMenu = nullptr;
            }
        }
    }

    void ProjectPanel::ShowDeleteConfirmation()
    {
        // Always center the confirmation dialog
        ImGui::OpenPopup("Delete?");
        ImVec2 center = ImGui::GetMainViewport()->GetCenter();
        ImGui::SetNextWindowPos(center, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));

        if (ImGui::BeginPopupModal("Delete?", NULL, ImGuiWindowFlags_AlwaysAutoResize))
        {
            ImGui::Text("Are you sure you want to delete '%s'?", m_NodeToDelete->Name.c_str());
            ImGui::Separator();

            if (ImGui::Button("Delete", ImVec2(120, 0))) {
                DeleteResource(*m_NodeToDelete);
                ImGui::CloseCurrentPopup();
                m_NodeToDelete = nullptr;
            }

            ImGui::SameLine();
            if (ImGui::Button("Cancel", ImVec2(120, 0))) {
                ImGui::CloseCurrentPopup();
                m_NodeToDelete = nullptr;
            }

            ImGui::EndPopup();
        }
    }

    void ProjectPanel::CreateNewFolder()
    {
		// Current directory
		fs::path currDir = ResourceDB::UuidToInfo(m_CurrentDirectory->Uuid).Path;

		// Default folder name
		fs::path newFolderPath = currDir / "NewFolder";

		// Ensure unique folder name
		int counter = 1;
		while (fs::exists(newFolderPath)) {
			newFolderPath = currDir / ("NewFolder_" + std::to_string(counter++));
		}

		// Create the folder
		if (!fs::create_directory(newFolderPath)) {
			LH_CORE_ERROR("Failed to create folder: {0}", newFolderPath.string());
			return;
		}

		// Create .meta file
		fs::path metaPath = newFolderPath;
		metaPath += ".meta";
		UUID uuid;
		MetaFile meta(uuid); // New random UUID
		meta.Save(metaPath);

		// Register with resource database
		ResourceDB::RegisterAsset(newFolderPath, meta.GetUUID());

		// Add new directory node for the folder
        DirectoryNode* newFolder = new DirectoryNode();
        newFolder->Uuid = meta.GetUUID();
        newFolder->Name = newFolderPath.filename().string();
        newFolder->Type = ResourceType::Directory;
        newFolder->Parent = m_CurrentDirectory;
        m_CurrentDirectory->Directories.push_back(std::move(newFolder));

		LH_CORE_INFO("Created new folder: {0}", newFolderPath.string());
    }

    void ProjectPanel::CreateNewMaterial()
    {
        // Current directory
        fs::path currDir = ResourceDB::UuidToInfo(m_CurrentDirectory->Uuid).Path;

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

        // Shader (empty by default)
        materialData["shader"] = "";

        // Material properties
        materialData["render_mode"] = 0;  // Opaque by default
        materialData["alpha_cutoff"] = 0.5f;
        materialData["blend_src"] = static_cast<int>(RendererAPI::BlendFactor::SrcAlpha);
        materialData["blend_dst"] = static_cast<int>(RendererAPI::BlendFactor::OneMinusSrcAlpha);
        materialData["alpha_from_diffuse"] = 0;  // False

        // Base material parameters
        materialData["color"] = { 1.0f, 1.0f, 1.0f, 1.0f };  // White
        materialData["alpha"] = 1.0f;
        materialData["metal"] = 0.0f;
        materialData["rough"] = 0.5f;
        materialData["emissive"] = { 0.0f, 0.0f, 0.0f };  // No emission
        materialData["is_gloss"] = 0;  // False
        materialData["is_single_channel"] = 0;  // False

        // Subsurface scattering defaults
        materialData["subsurface"] = {
            {"color", {1.0f, 1.0f, 1.0f}},
            {"strength", 0.0f},
            {"thickness_scale", 1.0f}
        };

        // Empty texture maps array
        materialData["textures"] = nlohmann::json::array();

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
		MaterialLibrary::LoadOrGet(newMaterialPath);

        // Add new directory node for the material
        DirectoryNode* newMaterial = new DirectoryNode();
        newMaterial->Uuid = meta.GetUUID();
        newMaterial->Name = newMaterialPath.filename().stem().string();
        newMaterial->Type = ResourceType::Material;
        newMaterial->Parent = m_CurrentDirectory;
        m_CurrentDirectory->Contents.push_back(std::move(newMaterial));

        // Set as selected in inspector
        m_InspectorPanel->SetSelectedResource(meta.GetUUID());

        // Notify listeners
        /*if (OnMaterialCreated) {
            OnMaterialCreated(newMaterialPath);
        }*/

        LH_CORE_INFO("Created new material: {0}", newMaterialPath.string());
    }

    void ProjectPanel::DeleteResource(DirectoryNode& nodeToDelete)
    {
		fs::path path = ResourceDB::UuidToInfo(nodeToDelete.Uuid).Path;

        try {
            // Delete the file or directory
            if (nodeToDelete.Type == ResourceType::Directory) {
                fs::remove_all(path);
            }
            else {
                fs::remove(path);
            }

            // Delete associated .meta
            fs::path metaPath = path.string() + ".meta";
            fs::remove(metaPath);
        }
        catch (const std::exception& e) {
			LH_CORE_ERROR("Failed to delete resource: {0}", e.what());
            return;
        }

        // Remove node
		bool found = DeleteNode(*m_CurrentDirectory, nodeToDelete);

        // Update UI state if needed
        if (m_SelectedNode == &nodeToDelete) {
            m_SelectedNode = nullptr;
            if (m_InspectorPanel) {
                m_InspectorPanel->SetSelectedResourceNone();
            }
        }

        if (m_NodeToRename == &nodeToDelete) {
            m_NodeToRename = nullptr;
        }
    }

    void ProjectPanel::RenameResource(DirectoryNode& node, const std::string& newName)
    {
        fs::path oldPath = ResourceDB::UuidToInfo(node.Uuid).Path;
        std::string extension = oldPath.extension().string();
        fs::path newPath = oldPath.parent_path() / (newName + extension);

        if (!fs::exists(oldPath)) {
            LH_CORE_ERROR("Resource to rename does not exist: {0}", oldPath.string());
            return;
        }

        try {
            // Rename the main file
            fs::rename(oldPath, newPath);

            // Rename .meta file if exists
            fs::path oldMetaPath = oldPath;
            oldMetaPath += ".meta";
            if (fs::exists(oldMetaPath)) {
                fs::path newMetaPath = newPath;
                newMetaPath += ".meta";
                fs::rename(oldMetaPath, newMetaPath);
            }

            // Update resource database
            ResourceDB::UpdateAssetPath(oldPath, newPath);
            node.Name = newName;

            switch (node.Type) {
                case ResourceType::Model:    ModelLibrary::Get(node.Uuid)->SetName(newName);    break;
                case ResourceType::Texture:  TextureCache::Get(node.Uuid)->SetName(newName);    break;
                case ResourceType::Material: MaterialLibrary::Get(node.Uuid)->SetName(newName); break;
                case ResourceType::Shader:   ShaderLibrary::Get(node.Uuid)->SetName(newName);   break;
                default: break;
            }

            LH_CORE_INFO("Renamed {0} to {1}", oldPath.filename().string(), newName + extension);
        }
        catch (const fs::filesystem_error& e) {
            LH_CORE_ERROR("Failed to rename resource: {0}", e.what());
        }
    }

    void ProjectPanel::DeleteDirectoryRecursive(const fs::path& path)
    {
        try {
            if (fs::exists(path)) {
                fs::remove_all(path);
                ResourceDB::UnregisterAsset(path);
                LH_CORE_INFO("Deleted directory: {0}", path.string());
            }
        }
        catch (const fs::filesystem_error& e) {
            LH_CORE_ERROR("Failed to delete directory: {0}", e.what());
        }
    }
}
