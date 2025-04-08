#include "luthpch.h"
#include "luth/scene/Systems.h"
#include "luth/editor/Editor.h"
#include "luth/editor/panels/HierarchyPanel.h"

namespace Luth
{
    // Static member definitions
    std::vector<std::shared_ptr<System>> Systems::s_Systems;
    std::shared_ptr<entt::registry> Systems::s_Registry;

    void Systems::Init() {
        LH_CORE_INFO("Initializing Systems...");
        AddSystem<RenderingSystem>();
    }

    void Systems::Shutdown() {
        s_Systems.clear();
        s_Registry.reset();
    }

    void Systems::Update() {
        if (!s_Registry) return;
        for (auto& system : s_Systems) {
            system->Update(*s_Registry);
        }
    }
}
