#pragma once

#include "luthien/Editor.h"
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

    // BuildDirectoryTree + UpdateSearchResults (both heavy CPU work in Tracy captures) still run in
    // OnDraw; moving them onto the gather fiber is the open optimization here.
    struct ProjectSnapshot {};

    class ProjectPanel : public Panel
    {
    public:
        ProjectPanel();

        void OnInit() override;
        void OnGather(EditorSnapshotBuilder& builder) override;
        void OnDraw(const EditorSnapshot& snapshot) override;

        void Refresh();

        float GetThumbnailSize() const { return m_ThumbnailSize; }
        void  SetThumbnailSize(float size) { m_ThumbnailSize = std::clamp(size, 16.0f, 96.0f); }

        fs::path GetCurrentDirectory() const { return m_CurrentDirNode ? m_CurrentDirNode->Path : m_AssetsPath; }

    private:
        std::unique_ptr<DirectoryNode> BuildDirectoryTree(const fs::path& path, DirectoryNode* parent);
        void UpdateSearchResults();
        void RecursiveSearch(DirectoryNode* node, const std::string& query);

        void DrawTree();
        void DrawTreeNode(DirectoryNode* node);
        void DrawContent();
        void DrawPathBar(float width);
        void DrawItem(DirectoryNode* node, bool isGrid);

        void HandleDragDrop(DirectoryNode* node);
        void HandleClick(DirectoryNode* node, bool doubleClick);
        void HandleContextMenu(DirectoryNode* node);
        void HandleRenaming();

        void CreateNewFolder();
        void CreateNewMaterial();
        void CreateNewPhysicsMaterial();
        void DeleteItem(DirectoryNode* node);
        void RenameItem(DirectoryNode* node, const std::string& newName);

        const char* GetIcon(AssetType type, bool isDirectory) const;

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
        float m_Padding = 4.0f;
        // Slider min stays 16; threshold raised so list view covers 16-32 and
        // grid mode starts at 33 (smallest grid icons used to be unusably tiny).
        static constexpr float k_ListModeThreshold = 32.0f;
        static constexpr int   k_MaxNameLines      = 3;     // grid-mode label cap, Unreal-style

        bool m_NeedsRefresh = false;
    };
}
