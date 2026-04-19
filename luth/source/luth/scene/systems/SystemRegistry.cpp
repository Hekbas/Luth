#include "luthpch.h"
#include "luth/scene/systems/SystemRegistry.h"
#include "luth/scene/systems/TransformSystem.h"
#include "luth/scene/systems/AnimationSystem.h"
#include "luth/scene/systems/CameraSystem.h"
#include "luth/scene/systems/LightingSystem.h"
#include "luth/scene/systems/RenderingSystem.h"

namespace Luth
{
    std::vector<std::unique_ptr<ISystem>> SystemRegistry::s_Systems;
    Scene* SystemRegistry::s_Scene = nullptr;

    void SystemRegistry::Init() {
        LH_CORE_INFO("Initializing Systems...");
        AddSystem<TransformSystem>();
        AddSystem<AnimationSystem>();
        AddSystem<CameraSystem>();
        AddSystem<LightingSystem>();
        AddSystem<RenderingSystem>();
    }

    void SystemRegistry::Shutdown() {
        s_Systems.clear();
        s_Scene = nullptr;
    }

    void SystemRegistry::Update() {
        if (!s_Scene) return;
        for (auto& system : s_Systems) {
            system->Update(s_Scene);
        }
    }
}
