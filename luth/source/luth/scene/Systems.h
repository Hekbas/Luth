#pragma once

#include "luth/scene/System.h"

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

        static void SetRegistry(std::shared_ptr<entt::registry> registry) { s_Registry = registry; }

    private:
        static std::vector<std::unique_ptr<System>> s_Systems;
        static std::shared_ptr<entt::registry> s_Registry;
    };
}
