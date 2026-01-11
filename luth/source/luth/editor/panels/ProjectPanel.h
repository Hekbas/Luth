#pragma once

#include "luth/editor/Editor.h"
#include "luth/resources/FileSystem.h"
#include "luth/core/UUID.h"

#include <memory>
#include <vector>
#include <filesystem>

namespace Luth
{
    struct DirectoryNode {
        fs::path Path;
        std::string Name;
        AssetType Type = AssetType::None;
        UUID Handle = UUID::Invalid();
        
        DirectoryNode* Parent = nullptr;
        std::vector<std::unique_ptr<DirectoryNode>> SubDirectories;
        std::vector<std::unique_ptr<DirectoryNode>> Files;
        
        bool IsOpen = false;
    };

	class InspectorPanel;

    class ProjectPanel : public Panel
    {
    public:
        ProjectPanel();

        void OnInit() override;
        void OnRender() override;

        void Refresh();

    private:
        std::unique_ptr<DirectoryNode> BuildDirectoryTree(const fs::path& path, DirectoryNode* parent);
        void UpdateSearchResults();
        void RecursiveSearch(DirectoryNode* node, const std::string& query);

        // UI rendering
        void DrawTree();
        void DrawTreeNode(DirectoryNode* node);
        void DrawContent();
        void DrawPathBar(float width);

        void DrawItem(DirectoryNode* node, bool isGrid);

        // Interaction
        void HandleDragDrop(DirectoryNode* node);
        void HandleClick(DirectoryNode* node, bool doubleClick);
        void HandleContextMenu(DirectoryNode* node);
        void HandleRenaming();

        // Actions
        void CreateNewFolder();
        void CreateNewMaterial();
        void DeleteItem(DirectoryNode* node);
        void RenameItem(DirectoryNode* node, const std::string& newName);

        const char* GetIcon(AssetType type, bool isDirectory) const;

        // Runtime state
		InspectorPanel* m_InspectorPanel = nullptr;
        fs::path m_AssetsPath;

        std::unique_ptr<DirectoryNode> m_RootNode;
        DirectoryNode* m_CurrentDirNode = nullptr;
        
        fs::path m_SelectedPath;
        DirectoryNode* m_RenamingNode = nullptr;
        char m_RenameBuffer[256] = "";
        char m_SearchBuffer[256] = "";
        std::vector<DirectoryNode*> m_SearchResults;
        bool m_IsSearching = false;

        float m_ThumbnailSize = 64.0f;
        float m_Padding = 16.0f;
        static constexpr float k_ListModeThreshold = 16.0f;
    };
}
