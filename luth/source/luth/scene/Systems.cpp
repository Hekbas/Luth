#include "luthpch.h"
#include "luth/scene/Systems.h"
#include "luth/editor/Editor.h"
#include "luth/editor/panels/HierarchyPanel.h"

namespace Luth
{
    // Static member definitions
    std::vector<std::unique_ptr<System>> Systems::s_Systems;
    std::shared_ptr<entt::registry> Systems::s_Registry;

    void Systems::Init() {
        s_Registry = Editor::GetPanel<HierarchyPanel>()->GetContext()->RegistryPtr();
        AddSystem<RenderingSystem>();
    }

    void Systems::Shutdown() {
        s_Systems.clear();
        s_Registry.reset();
    }

    void Systems::Update() {
        for (auto& system : s_Systems) {
            system->Update(*s_Registry);
        }
    }
}
