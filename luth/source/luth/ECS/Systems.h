#pragma once

#include "luth/ECS/System.h"

#include <vector>
#include <memory>

namespace Luth
{
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
                system->Update(*s_Registry);
            }
        }

        static void SetRegistry(std::shared_ptr<entt::registry> registry) { s_Registry = registry; }
        static entt::registry& GetRegistry() { return *s_Registry; }

    private:
        static std::vector<std::shared_ptr<System>> s_Systems;
        static std::shared_ptr<entt::registry> s_Registry;
    };
}
