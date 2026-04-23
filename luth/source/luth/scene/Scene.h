#pragma once

#include "luth/core/UUID.h"
#include "luth/resources/Asset.h"
#include <string>
#include <unordered_map>
#include <memory>
#include <entt/entt.hpp>

namespace Luth
{
    class Entity;

    class Scene
    {
    public:
        Scene();
        ~Scene();

        Entity CreateEntity(const std::string& name = "Entity");
        void DestroyEntity(Entity entity);
        void Clear();
        void ClearPreservingAssets();
        Entity DuplicateEntity(Entity original, bool skipParentAddition = false);
        
        void ReorderEntity(Entity entity, Entity target, bool after);

        entt::registry& Registry() { return m_Registry; }
        const entt::registry& Registry() const { return m_Registry; }
        std::shared_ptr<entt::registry> RegistryPtr() {
            return std::shared_ptr<entt::registry>(&m_Registry, [](entt::registry*) {});
        }

        template<typename Func>
        void EachEntity(Func func) {
            auto view = m_Registry.view<entt::entity>();
            view.each([this, func](entt::entity entityID) {
                func(Entity{ entityID, this });
            });
        }

        template<typename... Components>
        auto GetAllEntitiesWith() { return m_Registry.view<Components...>(); }

        const std::vector<Entity>& GetRootEntities() const { return m_RootEntities; }

        u32 GetHierarchyVersion() const { return m_HierarchyVersion; }
        void IncrementHierarchyVersion() { m_HierarchyVersion++; }

        // Asset lifetime: keeps shared_ptrs alive so Trim() won't evict in-use assets
        void HoldAsset(UUID uuid, std::shared_ptr<Asset> asset);
        void ReleaseAsset(UUID uuid);
        void ReleaseAllAssets();

        // UUID lookup (O(n) — editor-only, used by undo/redo)
        Entity FindEntityByUUID(UUID uuid) const;

        // Root entity list management (public for undo/redo command utils)
        void AddToRoots(Entity entity);
        void RemoveFromRoots(Entity entity);

    private:
        std::string GenerateUniqueName(Entity entity);

    private:
        entt::registry m_Registry;
        std::vector<Entity> m_RootEntities;
        u32 m_HierarchyVersion = 0;
        std::unordered_map<UUID, std::shared_ptr<Asset>, UUIDHash> m_HeldAssets;
        
        friend class Entity;
    };
}
