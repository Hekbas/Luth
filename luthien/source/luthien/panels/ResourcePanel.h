#pragma once

#include "luth/core/UUID.h"
#include "luthien/Editor.h"

#include <string>
#include <vector>
#include <unordered_map>

namespace Luth
{
    // Flat list view of every asset in the project plus the engine built-ins. Refresh fires off
    // the AssetDatabase ChangeCallback (m_NeedsRebuild flag); search and filter operate over the
    // cached m_FilteredResources so each ImGui draw doesn't re-query the database.
    struct ResourceListSnapshot {};  // PopulateData stays inline

    class ResourcePanel : public Panel
    {
    public:
        struct ResourceEntry
        {
            std::string Name;
            UUID Uuid;
            std::string Type;
            int RefCount;
        };

        ResourcePanel();

        void OnInit() override;
        void OnGather(EditorSnapshotBuilder& builder) override;
        void OnDraw(const EditorSnapshot& snapshot) override;

    private:
        void DrawFilterControls();
        void SetupColumns();

        void RebuildIfDirty();
        void RefreshDynamicData();
        void PopulateData();
        void AddModelEntries();
        void AddTextureEntries();
        void AddMaterialEntries();
        void AddShaderEntries();

        bool ResourceMatchesSearch(ResourceEntry entry);

        ImVec4 GetTypeColor(const std::string& type) const;

    private:
        char m_SearchBuffer[256] = "";
        bool m_ShowModels           = true;
        bool m_ShowTextures         = true;
        bool m_ShowMaterials        = true;
        bool m_ShowPhysicsMaterials = true;
        bool m_ShowShaders          = true;
        bool m_ShowFonts            = true;
        bool m_ShowScenes           = true;

        UUID m_SelectedUUID = UUID::Invalid();

        std::vector<ResourceEntry> m_FilteredResources;
        // invariant: m_FilteredResources is rebuilt only when m_NeedsRebuild flips.
        // Sources of truth: AssetDatabase callback, filter/search edits, sort spec.
        bool m_NeedsRebuild = true;

        static const std::unordered_map<std::string, ImVec4> m_TypeColors;

        const char* GetTypeIcon(const std::string& type) const;
    };
}
