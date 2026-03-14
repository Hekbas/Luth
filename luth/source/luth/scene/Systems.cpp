#include "luthpch.h"
#include "luth/scene/Systems.h"
#include "luth/scene/systems/TransformSystem.h"
#include "luth/scene/systems/AnimationSystem.h"
#include "luth/scene/systems/CameraSystem.h"
#include "luth/scene/systems/RenderingSystem.h"

namespace Luth
{
    std::vector<std::shared_ptr<System>> Systems::s_Systems;
    Scene* Systems::s_Scene = nullptr;

    void Systems::Init() {
        LH_CORE_INFO("Initializing Systems...");
        AddSystem<TransformSystem>();
        AddSystem<CameraSystem>();
        //AddSystem<AnimationSystem>();
        AddSystem<RenderingSystem>();
    }

    void Systems::Shutdown() {
        s_Systems.clear();
        s_Scene = nullptr;
    }

    void Systems::Update() {
        if (!s_Scene) return;
        for (auto& system : s_Systems) {
            system->Update(s_Scene);
        }
    }
}
