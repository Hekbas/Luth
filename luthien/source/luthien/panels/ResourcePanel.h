#pragma once

#include "luth/core/UUID.h"
#include "luthien/Editor.h"

#include <string>
#include <vector>
#include <unordered_map>

namespace Luth
{
    struct ResourceListSnapshot { /* placeholder; PopulateData stays inline for v2.9.0 */ };

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
        bool UsesNewLifecycle() const override { return true; }
        void OnGather(EditorSnapshotBuilder& builder) override;
        void OnDraw(const EditorSnapshot& snapshot) override;

    private:
        void DrawFilterControls();
        void SetupColumns();

        void PopulateData();
        void AddModelEntries();
        void AddTextureEntries();
        void AddMaterialEntries();
        void AddShaderEntries();

        bool ResourceMatchesSearch(ResourceEntry entry);

        ImVec4 GetTypeColor(const std::string& type) const;

    private:
        char m_SearchBuffer[256] = "";
        bool m_ShowModels    = true;
        bool m_ShowTextures  = true;
        bool m_ShowMaterials = true;
        bool m_ShowShaders   = true;
        bool m_ShowFonts     = true;
        bool m_ShowScenes    = true;

        UUID m_SelectedUUID = UUID::Invalid();

        std::vector<ResourceEntry> m_FilteredResources;

        static const std::unordered_map<std::string, ImVec4> m_TypeColors;

        const char* GetTypeIcon(const std::string& type) const;
    };
}
