#pragma once

#include "luth/scene/System.h"

#include <vector>
#include <memory>

namespace Luth
{
    class Scene;

    class Systems
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
        static std::shared_ptr<T> GetSystem() {
            for (auto& system : s_Systems) {
                if (auto found = std::dynamic_pointer_cast<T>(system))
                    return found;
            }
            return {};
        }

        template<typename T>
        static void Update() {
            if (auto system = GetSystem<T>()) {
                system->Update(s_Scene);
            }
        }

        static void SetScene(Scene* scene) { s_Scene = scene; }
        static Scene* GetScene() { return s_Scene; }

    private:
        static std::vector<std::shared_ptr<System>> s_Systems;
        static Scene* s_Scene;
    };
}
