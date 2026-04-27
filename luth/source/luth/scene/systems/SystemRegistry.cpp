#include "luthpch.h"
#include "luth/scene/systems/SystemRegistry.h"
#include "luth/scene/systems/TransformSystem.h"
#include "luth/scene/systems/AnimationSystem.h"
#include "luth/scene/systems/CameraSystem.h"
#include "luth/scene/systems/LightingSystem.h"
#include "luth/scene/systems/RenderingSystem.h"
#include "luth/scene/systems/PickingSystem.h"

namespace Luth
{
    std::vector<std::unique_ptr<ISystem>> SystemRegistry::s_Systems;
    std::vector<std::pair<size_t, ISystem*>> SystemRegistry::s_Slots;
    Scene* SystemRegistry::s_Scene = nullptr;

    void SystemRegistry::Init() {
        LH_CORE_INFO("Initializing Systems...");
        AddSystem<TransformSystem>();
        AddSystem<AnimationSystem>();
        AddSystem<CameraSystem>();
        AddSystem<LightingSystem>();
        AddSystem<RenderingSystem>();
        AddSystem<PickingSystem>();
    }

    void SystemRegistry::Shutdown() {
        s_Slots.clear();
        s_Systems.clear();
        s_Scene = nullptr;
    }

    void SystemRegistry::Update() {
        if (!s_Scene) return;
        for (size_t i = 0; i < s_Systems.size(); ++i) {
            s_Systems[i]->Update(s_Scene);
        }
    }
}
