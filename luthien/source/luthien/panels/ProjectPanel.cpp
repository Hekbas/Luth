#include "lepch.h"
#include "luthien/panels/ProjectPanel.h"
#include "luthien/EditorColors.h"
#include "luthien/EditorSelection.h"
#include "luthien/EditorSnapshot.h"
#include "luthien/Editor.h"
#include "luth/resources/AssetDatabase.h"
#include "luth/resources/AssetManager.h"
#include "luth/resources/MetaFile.h"
#include "luth/renderer/material/Material.h"
#include "luthien/widgets/ImGuiUtils.h"
#include "luthien/widgets/Icons.h"
#include "luthien/widgets/ThumbnailCache.h"

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
        if (!FileSystem::HasProject()) return;

        m_AssetsPath = FileSystem::AssetsPath();
        Refresh();

        // Register for hot-reload notifications from the file watcher
        AssetDatabase::AddChangeCallback([this]() { m_NeedsRefresh = true; });
        AssetDatabase::StartWatching();
    }

    static DirectoryNode* FindNodeByPath(DirectoryNode* root, const fs::path& target)
    {
        if (!root || target.empty()) return nullptr;
        if (root->Path == target) return root;
        for (auto& sub : root->SubDirectories) {
            if (auto* found = FindNodeByPath(sub.get(), target))
                return found;
        }
        return nullptr;
    }

    void ProjectPanel::Refresh()
    {
        // Re-read assets path (may have changed due to project switch)
        m_AssetsPath = FileSystem::AssetsPath();

        // Preserve current navigation position across tree rebuilds
        fs::path currentPath = m_CurrentDirNode ? m_CurrentDirNode->Path : fs::path{};

        m_RootNode = BuildDirectoryTree(m_AssetsPath, nullptr);

        m_CurrentDirNode = FindNodeByPath(m_RootNode.get(), currentPath);
        if (!m_CurrentDirNode)
            m_CurrentDirNode = m_RootNode.get();

        if (m_IsSearching)
            UpdateSearchResults();
    }

    void ProjectPanel::OnGather(EditorSnapshotBuilder& builder)
    {
        // Real opportunity site for follow-on polish: BuildDirectoryTree (recursive
        // filesystem walk on Refresh) + UpdateSearchResults (lowercase + substring on
        // every keystroke) both run today inside OnDraw and dominated the pre-rework
        // Tracy capture. v2.9.0 ships the structural migration; the actual work move
        // is a separate epic (editor-project-async-index).
        builder.Add<ProjectSnapshot>();
    }

    void ProjectPanel::OnDraw(const EditorSnapshot& /*snapshot*/)
    {
        LH_PROFILE_FUNCTION();
        ImGui::PushFont(Editor::GetFASolid());
        std::string project = ICON_FA_FOLDER + std::string("  Project");

        // Outer project window: 0 padding so child borders sit flush against
        // the panel chrome. Child windows below set their own internal padding.
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
        const bool windowOpen = BeginWindow(project.c_str());
        ImGui::PopStyleVar();
        if (windowOpen)
        {
            if (!FileSystem::HasProject())
            {
                ImGui::PopFont();
                float avail = ImGui::GetContentRegionAvail().y;
                ImGui::SetCursorPosY(ImGui::GetCursorPosY() + avail * 0.4f);
                float w = ImGui::CalcTextSize("No project loaded").x;
                ImGui::SetCursorPosX((ImGui::GetContentRegionAvail().x - w) * 0.5f);
                ImGui::TextDisabled("No project loaded");
                ImGui::End();
                return;
            }

            if (m_NeedsRefresh) {
                Refresh();
                m_NeedsRefresh = false;
            }
            float availWidth = ImGui::GetContentRegionAvail().x;
            float spacing = ImGui::GetStyle().ItemSpacing.x;
            float sliderWidth = std::min(availWidth * 0.1f, 100.0f);
            float pathBarWidth = availWidth - sliderWidth - spacing;
            if (pathBarWidth < 10.0f) pathBarWidth = 10.0f;

            // Top bar with path and slider
            DrawPathBar(pathBarWidth);
            ImGui::SameLine();
            
            ImGui::BeginChild("##SliderBar", ImVec2(sliderWidth, ImGui::GetFontSize() + ImGui::GetStyle().FramePadding.y * 2), ImGuiChildFlags_Border);
            ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x);
            ImGui::SliderFloat("##Size", &m_ThumbnailSize, 16.0f, 96.0f, "");
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Icon Size");
            ImGui::EndChild();

            // Left panel - directory tree
            ImGui::BeginChild("##ProjectTree", ImVec2(ImGui::GetWindowWidth() * 0.2f, 0), ImGuiChildFlags_Border | ImGuiChildFlags_ResizeX);
            
            ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x);
            if (ImGui::InputTextWithHint("##Search", ICON_FA_MAGNIFYING_GLASS " Search...", m_SearchBuffer, sizeof(m_SearchBuffer))) {
                m_IsSearching = strlen(m_SearchBuffer) > 0;
                UpdateSearchResults();
            }
            ImGui::Separator();

            ImGui::BeginChild("##TreeScroll", ImVec2(0, 0));
            DrawTree();
            ImGui::EndChild();

            ImGui::EndChild();

            ImGui::SameLine();

            // Right panel - directory contents. Internal WindowPadding gives
            // the grid breathing room from the child border; ItemSpacing.y
            // adds a small gap between rows.
            const ImVec2 prevSpacing = ImGui::GetStyle().ItemSpacing;
            ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(8.0f, 8.0f));
            ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing,   ImVec2(prevSpacing.x, 4.0f));
            ImGui::BeginChild("##ProjectContent", ImVec2(0, 0), ImGuiChildFlags_Border);

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
            ImGui::PopStyleVar(2);
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
        for (auto& dir : node->SubDirectories) {
            std::string dirName = dir->Name;
            std::transform(dirName.begin(), dirName.end(), dirName.begin(), ::tolower);
            if (dirName.find(query) != std::string::npos)
                m_SearchResults.push_back(dir.get());
            RecursiveSearch(dir.get(), query);
        }
        for (auto& file : node->Files) {
            std::string name = file->Name;
            std::transform(name.begin(), name.end(), name.begin(), ::tolower);
            if (name.find(query) != std::string::npos)
                m_SearchResults.push_back(file.get());
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
        const ImColor treeLineColor = EditorColors::TreeLineProject;
        const float smallOffsetX = -6.0f;
        ImVec2 verticalLineStart = ImGui::GetCursorScreenPos();
        ImDrawList* drawList = ImGui::GetWindowDrawList();

        if (node->IsOpen) {
            verticalLineStart.x += smallOffsetX; // My ocd will kill me
            ImVec2 verticalLineEnd = verticalLineStart;

            for (auto& child : node->SubDirectories) {
                auto currentPos = ImGui::GetCursorScreenPos();
                DrawTreeNode(child.get());
                verticalLineEnd.y = currentPos.y + ImGui::GetFontSize() * 0.5f;
            }

            // Draw vertical line
            drawList->AddLine(verticalLineStart, verticalLineEnd, treeLineColor);

            ImGui::TreePop();
        }
    }

    void ProjectPanel::DrawPathBar(float width)
    {
        if (!m_CurrentDirNode) return;

        ImGui::BeginChild("##PathBar", ImVec2(width, ImGui::GetFontSize() + ImGui::GetStyle().FramePadding.y * 2), ImGuiChildFlags_Border);

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

    namespace
    {
        // Word-wraps `text` to `wrapWidth` and renders up to `maxLines` lines.
        // Overflowing text on the last drawn line is replaced with "<chunk>..."
        // sized to fit the wrap width — mirrors Unreal's project-browser cap.
        void DrawTruncatedTextWrapped(const std::string& text, float wrapWidth, int maxLines)
        {
            if (text.empty() || maxLines <= 0 || wrapWidth <= 0.0f) return;
            ImFont* font = ImGui::GetFont();
            const char* p   = text.c_str();
            const char* end = p + text.size();
            int drawn = 0;
            while (p < end && drawn < maxLines)
            {
                const char* nextWrap = font->CalcWordWrapPositionA(1.0f, p, end, wrapWidth);
                if (nextWrap == p) nextWrap = p + 1;
                const bool needsTruncate = (drawn == maxLines - 1) && (nextWrap < end);
                if (!needsTruncate) {
                    ImGui::TextUnformatted(p, nextWrap);
                } else {
                    std::string line(p, nextWrap);
                    std::string display = line + "...";
                    while (ImGui::CalcTextSize(display.c_str()).x > wrapWidth && !line.empty()) {
                        line.pop_back();
                        display = line + "...";
                    }
                    ImGui::TextUnformatted(display.c_str());
                }
                p = nextWrap;
                while (p < end && (*p == ' ' || *p == '\n')) ++p;
                ++drawn;
            }
        }

        // Centers an FA glyph at `pos` within a `size` x `size` square. Caller
        // pushes the appropriate font + color before calling.
        void DrawIconCentered(const char* glyph, ImVec2 pos, float size)
        {
            const ImVec2 g = ImGui::CalcTextSize(glyph);
            ImGui::SetCursorScreenPos(ImVec2(pos.x + (size - g.x) * 0.5f,
                                             pos.y + (size - g.y) * 0.5f));
            ImGui::TextUnformatted(glyph);
        }
    }

    void ProjectPanel::DrawItem(DirectoryNode* node, bool isGrid)
    {
        const bool isDirectory = (node->Type == AssetType::None);
        const bool isSelected  = (m_SelectedPath == node->Path);
        const bool isRenaming  = (m_RenamingNode == node);
        const char* icon       = GetIcon(node->Type, isDirectory);

        const ImTextureID thumb = isDirectory
            ? (ImTextureID)0
            : UI::ThumbnailCache::Get(node->Handle, node->Type);

        const float lineH    = ImGui::GetTextLineHeight();
        // List mode picks a small thumbnail close to a single line.
        const float listThumbH = std::max(20.0f, lineH + 4.0f);

        const float cellW = isGrid ? m_ThumbnailSize : 0.0f;            // 0 → stretch column
        const float cellH = isGrid ? (m_ThumbnailSize + lineH * (float)k_MaxNameLines + 2.0f)
                                    : listThumbH;

        const ImVec2 startPos    = ImGui::GetCursorPos();
        const ImVec2 startScreen = ImGui::GetCursorScreenPos();

        // Selectable owns click + selection visual + drag-drop binding +
        // context menu binding for the entire cell. AllowOverlap lets the
        // image we draw on top stay hoverable for tooltips later if needed.
        if (!isRenaming) {
            ImGuiSelectableFlags flags = ImGuiSelectableFlags_AllowDoubleClick
                                       | ImGuiSelectableFlags_AllowOverlap;
            if (ImGui::Selectable("##sel", isSelected, flags, ImVec2(cellW, cellH))) {
                HandleClick(node, ImGui::IsMouseDoubleClicked(0));
            }
            HandleDragDrop(node);
            if (ImGui::BeginPopupContextItem()) {
                HandleContextMenu(node);
                ImGui::EndPopup();
            }
        }

        // Render visual content on top of the Selectable.
        ImGui::SetCursorPos(startPos);

        if (isGrid) {
            // ── Thumbnail / icon area (top, square of m_ThumbnailSize) ─────
            if (thumb != 0) {
                ImVec2 dims = UI::ThumbnailCache::GetThumbnailSize(node->Handle);
                float ar = (dims.y > 0.0f) ? (dims.x / dims.y) : 1.0f;
                ImVec2 disp = (ar >= 1.0f)
                    ? ImVec2(m_ThumbnailSize, m_ThumbnailSize / ar)
                    : ImVec2(m_ThumbnailSize * ar, m_ThumbnailSize);
                ImVec2 imgPos = ImVec2(startScreen.x + (m_ThumbnailSize - disp.x) * 0.5f,
                                       startScreen.y + (m_ThumbnailSize - disp.y) * 0.5f);
                ImGui::SetCursorScreenPos(imgPos);
                ImGui::Image(thumb, disp, { 0, 0 }, { 1, 1 });
            } else {
                if (isDirectory && node->SubDirectories.empty() && node->Files.empty())
                    ImGui::PushFont(Editor::GetFARegular());
                else
                    ImGui::PushFont(Editor::GetFASolid());
                if (!isDirectory) {
                    const Vec4 c = FileSystem::GetTypeInfo().at(node->Type).color;
                    ImGui::PushStyleColor(ImGuiCol_Text, { c.r, c.g, c.b, c.a });
                }
                DrawIconCentered(icon, startScreen, m_ThumbnailSize);
                if (!isDirectory) ImGui::PopStyleColor();
                ImGui::PopFont();
            }

            // ── Name area (3-line truncated) ─────────────────────────────
            ImGui::SetCursorScreenPos(ImVec2(startScreen.x, startScreen.y + m_ThumbnailSize + 2.0f));
            if (isRenaming) {
                ImGui::SetNextItemWidth(m_ThumbnailSize);
                HandleRenaming();
            } else {
                DrawTruncatedTextWrapped(node->Name, m_ThumbnailSize, k_MaxNameLines);
            }
        }
        else {  // List view
            // ── Thumbnail / colored icon (left, square of listThumbH) ────
            const float pad = 4.0f;
            ImVec2 imgPos = ImVec2(startScreen.x + pad, startScreen.y);
            if (thumb != 0) {
                ImVec2 dims = UI::ThumbnailCache::GetThumbnailSize(node->Handle);
                float ar = (dims.y > 0.0f) ? (dims.x / dims.y) : 1.0f;
                ImVec2 disp = (ar >= 1.0f)
                    ? ImVec2(listThumbH, listThumbH / ar)
                    : ImVec2(listThumbH * ar, listThumbH);
                ImVec2 centered = ImVec2(imgPos.x + (listThumbH - disp.x) * 0.5f,
                                         imgPos.y + (listThumbH - disp.y) * 0.5f);
                ImGui::SetCursorScreenPos(centered);
                ImGui::Image(thumb, disp, { 0, 0 }, { 1, 1 });
            } else {
                if (isDirectory && node->SubDirectories.empty() && node->Files.empty())
                    ImGui::PushFont(Editor::GetFARegular());
                else
                    ImGui::PushFont(Editor::GetFASolid());
                if (!isDirectory) {
                    const Vec4 c = FileSystem::GetTypeInfo().at(node->Type).color;
                    ImGui::PushStyleColor(ImGuiCol_Text, { c.r, c.g, c.b, c.a });
                }
                DrawIconCentered(icon, imgPos, listThumbH);
                if (!isDirectory) ImGui::PopStyleColor();
                ImGui::PopFont();
            }

            // ── Name (right of thumbnail, vertically centered) ────────────
            ImGui::SetCursorScreenPos(ImVec2(startScreen.x + pad + listThumbH + 6.0f,
                                             startScreen.y + (listThumbH - lineH) * 0.5f));
            if (isRenaming) {
                HandleRenaming();
            } else {
                ImGui::TextUnformatted(node->Name.c_str());
                if (m_IsSearching) {
                    ImGui::SameLine();
                    ImGui::TextDisabled("(%s)", node->Path.parent_path().string().c_str());
                }
            }
        }

        // Pin cursor to cell bottom so the next column / row starts cleanly.
        ImGui::SetCursorPos(ImVec2(startPos.x, startPos.y + cellH));
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
			{ AssetType::Scene,   ICON_FA_FILM                  },
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
                EditorSelection::SelectResource(node->Handle);
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
