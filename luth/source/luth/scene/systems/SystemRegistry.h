#pragma once

#include "luth/scene/systems/ISystem.h"

#include <memory>
#include <typeinfo>
#include <vector>

namespace Luth
{
    class Scene;

    // Typed-handle table for ECS systems. Registration order is update order. Get<T>() is
    // O(N) across slots, but N is small (~6 systems) and the lookup is rare. Holds the bound
    // Scene pointer so Update() doesn't have to re-thread it through every callsite.
    class SystemRegistry
    {
    public:
        static void Init();
        static void Shutdown();

        static void Update();

        template<typename T, typename... Args>
        static void AddSystem(Args&&... args) {
            auto sys = std::make_unique<T>(std::forward<Args>(args)...);
            T* raw = sys.get();
            s_Slots.emplace_back(typeid(T).hash_code(), raw);
            s_Systems.emplace_back(std::move(sys));
        }

        // Typed lookup: typeid hash keyed, linear slot scan, no dynamic_cast.
        template<typename T>
        static T* GetSystem() {
            const size_t h = typeid(T).hash_code();
            for (size_t i = 0; i < s_Slots.size(); ++i) {
                if (s_Slots[i].first == h)
                    return static_cast<T*>(s_Slots[i].second);
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
        static std::vector<std::pair<size_t, ISystem*>> s_Slots;
        static Scene* s_Scene;
    };
}
