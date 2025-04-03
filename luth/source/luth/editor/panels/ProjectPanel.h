#pragma once

#include "luth/editor/Editor.h"
#include "luth/resources/FileSystem.h"

namespace Luth
{
    constexpr const char* ASSET_UUID = "ASSET_UUID";
    constexpr const char* ENTITY_UUID = "ENTITY_UUID";

    struct DirectoryNode {
        UUID Uuid;
        std::string Name;
        ResourceType Type;
        std::vector<DirectoryNode> Directories;
        std::vector<DirectoryNode> Contents;
    };

    class ProjectPanel : public Panel
    {
    public:
        ProjectPanel();

        void OnInit() override;
        void OnRender() override;

    private:
        DirectoryNode BuildDirectoryTree(const fs::path& path);
        void DrawDirectoryNode(DirectoryNode& node);
        void DrawPathBar();
        void DrawDirectoryContent();
        void DrawCreateMenu();

        void CreateNewMaterial();

        const DirectoryNode* FindNodeByUuid(const DirectoryNode& node, const UUID& uuid);

        std::string m_AssetsPath;
        DirectoryNode m_RootNode;
        UUID m_CurrentDirectoryUuid;
    };
}
