#include "luthpch.h"
#include "luth/editor/panels/ProjectPanel.h"
#include "luth/editor/panels/InspectorPanel.h"
#include "luth/editor/Editor.h"
#include "luth/resources/AssetDatabase.h"
#include "luth/resources/AssetManager.h"
#include "luth/resources/MetaFile.h"
#include "luth/renderer/Material.h"
#include "luth/utils/ImGuiUtils.h"
#include "luth/utils/LuthIcons.h"

#include <nlohmann/json.hpp>
#include <fstream>

namespace Luth
{
    ProjectPanel::ProjectPanel()
    {
        LH_CORE_INFO("Created Project panel");
    }

    void ProjectPanel::OnInit()
    {
        m_InspectorPanel = Editor::GetPanel<InspectorPanel>();
        m_AssetsPath = FileSystem::AssetsPath();
        Refresh();
    }

    void ProjectPanel::Refresh()
    {
        m_RootNode = BuildDirectoryTree(m_AssetsPath, nullptr);
        m_CurrentDirNode = m_RootNode.get();
        if (m_IsSearching)
            UpdateSearchResults();
    }

    void ProjectPanel::OnRender()
    {
        ImGui::PushFont(Editor::GetFASolid());
        std::string project = ICON_FA_FOLDER + std::string("  Project");

        if (ImGui::Begin(project.c_str()))
        {
            // Left panel - directory tree
            ImGui::BeginChild("##ProjectTree", ImVec2(ImGui::GetWindowWidth() * 0.2f, 0), ImGuiChildFlags_ResizeX);
            
            ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x);
            if (ImGui::InputTextWithHint("##Search", ICON_FA_MAGNIFYING_GLASS " Search...", m_SearchBuffer, sizeof(m_SearchBuffer))) {
                m_IsSearching = strlen(m_SearchBuffer) > 0;
                UpdateSearchResults();
            }
            ImGui::Separator();

            DrawTree();
            ImGui::EndChild();

            ImGui::SameLine();

            // Right panel - Split view
            ImGui::PushStyleColor(ImGuiCol_ChildBg, IM_COL32(0, 0, 0, 0));
            ImGui::BeginChild("##ProjectSplitView", ImVec2(0, 0), true, ImGuiWindowFlags_HorizontalScrollbar);
            ImGui::PopStyleColor();

            // Top bar with path and slider
            float availWidth = ImGui::GetContentRegionAvail().x;
            float spacing = ImGui::GetStyle().ItemSpacing.x;
            float sliderWidth = std::min(availWidth * 0.1f, 100.0f);
            float pathBarWidth = availWidth - sliderWidth - spacing;
            if (pathBarWidth < 10.0f) pathBarWidth = 10.0f;

            DrawPathBar(pathBarWidth);
            ImGui::SameLine();
            
            ImGui::SetNextItemWidth(sliderWidth);
            ImGui::SliderFloat("##Size", &m_ThumbnailSize, 16.0f, 96.0f, "");
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Icon Size");

            // Directory contents
            ImGui::BeginChild("##ProjectContent", ImVec2(0, 0), true);

            if (ImGui::IsWindowHovered() && ImGui::GetIO().KeyCtrl)
            {
                float zoom = ImGui::GetIO().MouseWheel * 4.0f;
                if (zoom != 0.0f)
                {
                    m_ThumbnailSize = std::clamp(m_ThumbnailSize + zoom, 16.0f, 96.0f);
                }
            }

            DrawContent();
            ImGui::EndChild();

            ImGui::EndChild();
        }
        ImGui::End();
        ImGui::PopFont();
    }

    std::unique_ptr<DirectoryNode> ProjectPanel::BuildDirectoryTree(const fs::path& path, DirectoryNode* parent)
    {
        auto node = std::make_unique<DirectoryNode>();
        node->Path = path;
        node->Name = path.filename().string();
        node->Parent = parent;

        if (node->Name.empty()) {
            node->Name = "Assets";
            node->IsOpen = true;
        }

        try {
            for (const auto& entry : fs::directory_iterator(path)) {
                if (entry.path().extension() == ".meta") continue;
                
                if (entry.is_directory()) {
                    auto child = BuildDirectoryTree(entry.path(), node.get());
                    node->SubDirectories.push_back(std::move(child));
                }
                else {
                    AssetType fileType = FileSystem::ClassifyFileType(entry.path());
                    if (fileType != AssetType::None) {
						auto fileNode = std::make_unique<DirectoryNode>();
                        fileNode->Path = entry.path();
                        fileNode->Name = entry.path().filename().stem().string();
                        fileNode->Type = fileType;
                        fileNode->Handle = AssetDatabase::GetUUID(entry.path());
                        fileNode->Parent = node.get();
                        node->Files.push_back(std::move(fileNode));
                    }
                }
            }
        }
        catch (...) {
            // Handle errors
        }

        return node;
    }

    void ProjectPanel::UpdateSearchResults()
    {
        m_SearchResults.clear();
        if (m_IsSearching && m_RootNode) {
            std::string query = m_SearchBuffer;
            std::transform(query.begin(), query.end(), query.begin(), ::tolower);
            RecursiveSearch(m_RootNode.get(), query);
        }
    }

    void ProjectPanel::RecursiveSearch(DirectoryNode* node, const std::string& query)
    {
        for (auto& dir : node->SubDirectories) RecursiveSearch(dir.get(), query);
        for (auto& file : node->Files) {
            std::string name = file->Name;
            std::transform(name.begin(), name.end(), name.begin(), ::tolower);
            if (name.find(query) != std::string::npos) {
                m_SearchResults.push_back(file.get());
            }
        }
    }

    void ProjectPanel::DrawTree()
    {
        if (m_RootNode) {
            ImGui::SetNextItemOpen(true, ImGuiCond_FirstUseEver);
            DrawTreeNode(m_RootNode.get());
        }
    }

    void ProjectPanel::DrawTreeNode(DirectoryNode* node)
    {
        // Setup flags
        ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_SpanFullWidth |
            ImGuiTreeNodeFlags_OpenOnArrow | ImGuiTreeNodeFlags_OpenOnDoubleClick;
        
        if (node->SubDirectories.empty()) flags |= ImGuiTreeNodeFlags_Leaf;
        if (node == m_CurrentDirNode) flags |= ImGuiTreeNodeFlags_Selected;
        if (node->Name == "Assets") flags |= ImGuiTreeNodeFlags_Framed;

        if (node->IsOpen) ImGui::SetNextItemOpen(true);

        // Set Icon
        const char* icon = ICON_FA_FOLDER;
        if (node->SubDirectories.empty() && node->Files.empty()) {
            ImGui::PushFont(Editor::GetFARegular());
        }
        else if (node->IsOpen && !node->SubDirectories.empty()) {
            icon = ICON_FA_FOLDER_OPEN;
            ImGui::PushFont(Editor::GetFARegular());
		}
		else {
			ImGui::PushFont(Editor::GetFASolid());
        }
            
        // Draw the node
        node->IsOpen = ImGui::TreeNodeEx((void*)node, flags, "%s", icon);
        ImGui::PopFont();

        if (ImGui::IsItemClicked()) {
            m_CurrentDirNode = node;
            m_SelectedPath = node->Path;
        }

        ImGui::SameLine();
        ImGui::Text(node->Name.c_str());
        
        // Visual line settings
        const ImColor treeLineColor = ImColor(128, 128, 128, 128);
        const float smallOffsetX = -6.0f;
        ImVec2 verticalLineStart = ImGui::GetCursorScreenPos();
        ImDrawList* drawList = ImGui::GetWindowDrawList();

        if (node->IsOpen) {
            verticalLineStart.x += smallOffsetX; // My ocd will kill me
            ImVec2 verticalLineEnd = verticalLineStart;

            for (auto& child : node->SubDirectories) {
                auto currentPos = ImGui::GetCursorScreenPos();

                // Calculate horizontal line size
                float horizontalTreeLineSize = 20.0f;
                if (!child->SubDirectories.empty())
                    horizontalTreeLineSize *= 0.5f;

                // Draw child node
                DrawTreeNode(child.get());

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

    void ProjectPanel::DrawPathBar(float width)
    {
        if (!m_CurrentDirNode) return;

        ImGui::BeginChild("##PathBar", ImVec2(width, ImGui::GetFontSize() + ImGui::GetStyle().FramePadding.y * 2), false);

        // Store path segments in reverse order (from current to root)
        std::vector<DirectoryNode*> pathSegments;
        for (DirectoryNode* node = m_CurrentDirNode; node != nullptr; node = node->Parent) {
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
                    m_CurrentDirNode = segment;
                }
            }
            else {
                // Calculate text size for proper alignment
                const ImVec2 textSize = ImGui::CalcTextSize(segment->Name.c_str());

                // Use Selectable for clickable segments with proper sizing
                if (ImGui::Selectable(segment->Name.c_str(), false, 0, textSize)) {
                    m_CurrentDirNode = segment;
                }
            }

            isFirst = false;
        }

        ImGui::EndChild();
    }

    void ProjectPanel::DrawContent()
    {
        if (!m_CurrentDirNode && !m_IsSearching) return;

        if (ImGui::BeginPopupContextWindow("ProjectContextMenu")) {
            if (ImGui::MenuItem("Create Folder")) CreateNewFolder();
            if (ImGui::MenuItem("Create Material")) CreateNewMaterial();
            ImGui::EndPopup();
        }

        bool isListView = m_ThumbnailSize <= k_ListModeThreshold;
        float cellSize = m_ThumbnailSize + m_Padding;
        float panelWidth = ImGui::GetContentRegionAvail().x;
        int columnCount = (int)(panelWidth / cellSize);
        if (columnCount < 1) columnCount = 1;

        if (!isListView) {
            ImGui::Columns(columnCount, 0, false);
        }

        if (m_IsSearching)
        {
            for (auto* node : m_SearchResults) {
                ImGui::PushID(node);
                DrawItem(node, !isListView);
                ImGui::PopID();
                if (!isListView) ImGui::NextColumn();
            }
            
            if (m_SearchResults.empty()) {
                ImGui::TextDisabled("No results found.");
            }
        }
        else
        {
            // Directories
            for (auto& dir : m_CurrentDirNode->SubDirectories) {
                ImGui::PushID(dir.get());
                DrawItem(dir.get(), !isListView);
                ImGui::PopID();
                if (!isListView) ImGui::NextColumn();
            }

            // Files
            for (auto& file : m_CurrentDirNode->Files) {
                ImGui::PushID(file.get());
                DrawItem(file.get(), !isListView);
                ImGui::PopID();
                if (!isListView) ImGui::NextColumn();
            }
        }

        if (!isListView) {
            ImGui::Columns(1);
        }
    }

    void ProjectPanel::DrawItem(DirectoryNode* node, bool isGrid)
    {
        bool isDirectory = (node->Type == AssetType::None);
        const char* icon = GetIcon(node->Type, isDirectory);
        
        bool isSelected = (m_SelectedPath == node->Path);
        bool isRenaming = (m_RenamingNode == node);

        if (isGrid)
        {
            ImGui::BeginGroup();
            ImGui::PushFont(Editor::GetFASolid());
            
            // Colorize icon
            if (!isDirectory) {
                Vec4 color = FileSystem::GetTypeInfo().at(node->Type).color;
                ImGui::PushStyleColor(ImGuiCol_Text, { color.r, color.g, color.b, color.a });
            }

            // Icon Button
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0, 0, 0, 0));
            if (ImGui::Button(icon, { m_ThumbnailSize, m_ThumbnailSize })) {
                HandleClick(node, false);
            }
            ImGui::PopStyleColor();

            if (!isDirectory) ImGui::PopStyleColor();
            ImGui::PopFont();

            // Handle Drag Drop
            HandleDragDrop(node);
            
            // Handle Double Click
            if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(0)) {
                HandleClick(node, true);
            }

            // Context Menu
            if (ImGui::BeginPopupContextItem()) {
                HandleContextMenu(node);
                ImGui::EndPopup();
            }

            // Name
            if (isRenaming) {
                HandleRenaming();
            }
            else {
                ImGui::TextWrapped("%s", node->Name.c_str());
            }
            
            ImGui::EndGroup();
        }
        else // List View (Small Zoom)
        {
            ImGui::SetCursorPosX(ImGui::GetCursorPosX() + 8.0f);

            ImGui::PushFont(Editor::GetFASolid());
            if (!isDirectory) {
                Vec4 color = FileSystem::GetTypeInfo().at(node->Type).color;
                ImGui::PushStyleColor(ImGuiCol_Text, { color.r, color.g, color.b, color.a });
            }
            ImGui::Text(icon);
            if (!isDirectory) ImGui::PopStyleColor();
            ImGui::PopFont();
            // TODO: Add small icon texture support here if available

            ImGui::SameLine();

            if (isRenaming) {
                HandleRenaming();
            }
            else {
                if (ImGui::Selectable(node->Name.c_str(), isSelected, ImGuiSelectableFlags_AllowDoubleClick)) {
                    HandleClick(node, ImGui::IsMouseDoubleClicked(0));
                }
                
                HandleDragDrop(node);
                
                if (ImGui::BeginPopupContextItem()) {
                    HandleContextMenu(node);
                    ImGui::EndPopup();
                }
            }

            // In list view, show extra details if searching (like path)
            if (m_IsSearching) {
                ImGui::SameLine();
                ImGui::TextDisabled("(%s)", node->Path.parent_path().string().c_str());
            }
        }
    }

    const char* ProjectPanel::GetIcon(AssetType type, bool isDirectory) const
    {
        if (isDirectory) return ICON_FA_FOLDER;

        static const std::unordered_map<AssetType, const char*> icons = {
            { AssetType::Model,    ICON_FA_CUBE                  },
            { AssetType::Texture,  ICON_FA_IMAGE                 },
            { AssetType::Material, ICON_FA_CIRCLE_HALF_STROKE    },
			{ AssetType::Shader,   ICON_FA_FILE_CODE             },
			{ AssetType::Font,     ICON_FA_FONT                  },
			{ AssetType::None,  ICON_FA_FILE_CIRCLE_QUESTION  }
        };
        return icons.count(type) ? icons.at(type) : ICON_FILE;
    }

    void ProjectPanel::HandleDragDrop(DirectoryNode* node)
    {
        if (node->Type == AssetType::None) return; // Don't drag folders for now

        if (ImGui::BeginDragDropSource(ImGuiDragDropFlags_SourceAllowNullID)) {
            ImGui::SetDragDropPayload("ASSET_UUID", &node->Handle, sizeof(UUID));
            ImGui::Text("%s", node->Name.c_str());
            ImGui::EndDragDropSource();
        }
    }

    void ProjectPanel::HandleClick(DirectoryNode* node, bool doubleClick)
    {
        m_SelectedPath = node->Path;

        if (doubleClick) {
            if (node->Type == AssetType::None) {
                m_CurrentDirNode = node;
            }
            else if (node->Type == AssetType::Scene) {
                Editor::OpenScene(node->Path);
            }
        }
        else {
            if (node->Type != AssetType::None) {
                m_InspectorPanel->SetSelectedResource(node->Handle);
            }
        }
    }

    void ProjectPanel::HandleContextMenu(DirectoryNode* node)
    {
        if (ImGui::MenuItem("Rename")) {
            m_RenamingNode = node;
            strncpy_s(m_RenameBuffer, node->Name.c_str(), sizeof(m_RenameBuffer));
        }
        if (ImGui::MenuItem("Delete")) {
            DeleteItem(node);
        }
    }

    void ProjectPanel::HandleRenaming()
    {
        if (!m_RenamingNode) return;

        ImGui::SetKeyboardFocusHere();
        if (ImGui::InputText("##Rename", m_RenameBuffer, sizeof(m_RenameBuffer), ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_AutoSelectAll)) {
            RenameItem(m_RenamingNode, m_RenameBuffer);
            m_RenamingNode = nullptr;
        }
        
        if (!ImGui::IsItemActive() && (ImGui::IsMouseClicked(0) || ImGui::IsMouseClicked(1))) {
            m_RenamingNode = nullptr;
        }
    }

    void ProjectPanel::CreateNewFolder()
    {
        fs::path path = m_CurrentDirNode->Path / "New Folder";
        int i = 1;
        while (fs::exists(path)) {
            path = m_CurrentDirNode->Path / ("New Folder " + std::to_string(i++));
        }
        fs::create_directory(path);
        Refresh();
    }

    void ProjectPanel::CreateNewMaterial()
    {
        fs::path path = m_CurrentDirNode->Path / "New Material.mat";
        int counter = 1;
        while (fs::exists(path)) {
            path = m_CurrentDirNode->Path / ("New Material " + std::to_string(counter++) + ".mat");
        }

        // Default Material JSON
        nlohmann::json materialData;
        materialData["shader"] = "";
        materialData["render_mode"] = 0;
        materialData["alpha_cutoff"] = 0.5f;
        materialData["blend_src"] = static_cast<int>(Material::BlendFactor::SrcAlpha);
        materialData["blend_dst"] = static_cast<int>(Material::BlendFactor::OneMinusSrcAlpha);
        materialData["alpha_from_diffuse"] = 0;  // False
        materialData["textures"] = nlohmann::json::array();

        std::ofstream file(path);
        file << materialData.dump(4);
        file.close();

        // Register
        UUID uuid = MetaFile::Create(path, AssetType::Material);
        AssetDatabase::RegisterAsset(path, uuid, AssetType::Material);

        Refresh();
    }

    void ProjectPanel::DeleteItem(DirectoryNode* node)
    {
        if (node->Type == AssetType::None) {
            fs::remove_all(node->Path);
        }
        else {
            fs::remove(node->Path);
            fs::remove(node->Path.string() + ".meta");
            AssetDatabase::UnregisterAsset(node->Handle);
        }
        Refresh();
    }

    void ProjectPanel::RenameItem(DirectoryNode* node, const std::string& newName)
    {
        fs::path newPath = node->Path.parent_path() / newName;
        if (node->Type != AssetType::None) newPath += node->Path.extension();

        try {
            fs::rename(node->Path, newPath);
            
            if (node->Type != AssetType::None) {
                fs::rename(node->Path.string() + ".meta", newPath.string() + ".meta");
                AssetDatabase::UnregisterAsset(node->Handle);
                AssetDatabase::RegisterAsset(newPath, node->Handle, node->Type);
            }
            
            Refresh();
        }
        catch (std::exception& e) {
            LH_CORE_ERROR("Rename failed: {0}", e.what());
        }
    }
}
