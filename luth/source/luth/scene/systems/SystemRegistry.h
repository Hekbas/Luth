#pragma once

#include "luth/scene/systems/ISystem.h"

#include <vector>
#include <memory>

namespace Luth
{
    class Scene;

    class SystemRegistry
    {
    public:
        static void Init();
        static void Shutdown();

        static void Update();

        template<typename T, typename... Args>
        static void AddSystem(Args&&... args) {
            s_Systems.emplace_back(std::make_unique<T>(std::forward<Args>(args)...));
        }

        template<typename T>
        static T* GetSystem() {
            for (auto& system : s_Systems) {
                if (auto* found = dynamic_cast<T*>(system.get()))
                    return found;
            }
            return nullptr;
        }

        template<typename T>
        static void Update() {
            if (auto* system = GetSystem<T>()) {
                system->Update(s_Scene);
            }
        }

        static void SetScene(Scene* scene) { s_Scene = scene; }
        static Scene* GetScene() { return s_Scene; }

    private:
        static std::vector<std::unique_ptr<ISystem>> s_Systems;
        static Scene* s_Scene;
    };
}
