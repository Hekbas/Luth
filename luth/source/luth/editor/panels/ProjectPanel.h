#pragma once

#include "luth/editor/Editor.h"
#include "luth/resources/ResourceManager.h"

namespace Luth
{
    class ProjectPanel : public Panel
    {
    public:
        ProjectPanel();

        void OnInit() override;
        void OnRender() override;

    private:
        void DrawPathBar();
        void DrawDirectoryTree(const std::string& path);
        void DrawDirectoryContents();
        void DrawDirectoryNode(const fs::path& path, bool isRoot = false);

    private:
        std::string m_AssetsPath;
        std::string m_CurrentDirectory;
    };
}
