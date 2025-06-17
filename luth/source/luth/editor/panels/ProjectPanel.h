#pragma once

#include "luth/editor/Editor.h"
#include "luth/resources/FileSystem.h"

#include <memory>
#include <vector>

namespace Luth
{
    constexpr const char* ASSET_UUID = "ASSET_UUID";
    constexpr const char* ENTITY_UUID = "ENTITY_UUID";

    struct DirectoryNode {
        UUID Uuid;
        std::string Name;
        ResourceType Type;
        bool IsOpen = false;
        DirectoryNode* Parent = nullptr;
        std::vector<DirectoryNode*> Directories;
        std::vector<DirectoryNode*> Contents;

        /*bool operator==(const DirectoryNode& other) const {
            return Uuid == other.Uuid;
        }*/
    };

	class InspectorPanel;

    class ProjectPanel : public Panel
    {
    public:
        ProjectPanel();

        void OnInit() override;
        void OnRender() override;

    private:
        // Directory tree management
        DirectoryNode* BuildDirectoryTree(const fs::path& path, DirectoryNode* parent = nullptr);
        DirectoryNode* FindNode(DirectoryNode& root, const DirectoryNode& target);
        bool DeleteNode(DirectoryNode& root, const DirectoryNode& target);

        // UI rendering
        void DrawDirectoryNode(DirectoryNode& node);
        void DrawPathBar();
        void DrawDirectoryContent();

        void DrawListView();
        void DrawListItems(std::vector<DirectoryNode*>& items, bool isDirectory);
        void DrawListItem(DirectoryNode& item, bool isDirectory);

        void DrawGridView();
        void DrawGridItems(std::vector<DirectoryNode*>& items, bool isDirectory);
        void DrawGridItem(DirectoryNode& item, bool isDirectory);

        const char* GetResourceIcon(ResourceType type);
		void HandleDragDrop(DirectoryNode& item);
        void HandleItemInteraction(DirectoryNode& item, bool isDirectory);
        void HandleRenaming();

        void DrawCreateMenu();
        void ShowDeleteConfirmation();

        // Resource operations
        void CreateNewFolder();
        void CreateNewMaterial();
        void DeleteResource(DirectoryNode& parentNode);
        void RenameResource(DirectoryNode& node, const std::string& newName);
        void DeleteDirectoryRecursive(const fs::path& path);

        // Runtime state
		InspectorPanel* m_InspectorPanel = nullptr;
        std::string m_AssetsPath;

        DirectoryNode* m_RootNode;
        DirectoryNode* m_CurrentDirectory = nullptr;
        DirectoryNode* m_SelectedNode = nullptr;
        DirectoryNode* m_NodeMenu = nullptr;
        DirectoryNode* m_NodeToRename = nullptr;
        DirectoryNode* m_NodeToDelete = nullptr;

        char m_RenameBuffer[256] = "";
        std::string m_OriginalName;

		bool m_ListView = true;
    };
}
