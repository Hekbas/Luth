#include "luthpch.h"
#include "luth/scene/Scene.h"
#include "luth/scene/Components.h"
#include "luth/renderer/Material.h"
#include "luth/renderer/Texture.h"
#include "luth/resources/AssetManager.h"

namespace Luth
{
    using namespace Component;

    Scene::Scene()
    {
        LH_CORE_INFO("Created new scene");
    }

    Scene::~Scene()
    {
        m_Registry.clear();
        LH_CORE_INFO("Destroyed scene");
    }

    void Scene::Clear()
    {
        ReleaseAllAssets();
        m_RootEntities.clear();

        // Destroy every entity individually. This keeps the registry's
        // pool / sparse-set structures intact (non-zero bucket count)
        // so that subsequent view<>() calls don't hit EnTT's fast_mod
        // "power of two" assertion.  registry.clear() can reset bucket
        // counts to 0 in some EnTT versions, which is unsafe.
        std::vector<entt::entity> all;
        auto view = m_Registry.view<entt::entity>();
        view.each([&all](entt::entity e) { all.push_back(e); });
        for (auto e : all)
            m_Registry.destroy(e);

        IncrementHierarchyVersion();
        LH_CORE_TRACE("Scene cleared");
    }

    Entity Scene::CreateEntity(const std::string& name)
    {
        Entity entity = { m_Registry.create(), this };
        entity.AddComponent<ID>();
        entity.AddComponent<Tag>(name);
        entity.AddComponent<Transform>();
        entity.AddComponent<WorldTransform>();
        m_RootEntities.push_back(entity);
        LH_CORE_TRACE("Created entity: {0}", name);
        IncrementHierarchyVersion();
        return entity;
    }

    void Scene::DestroyEntity(Entity entity)
    {
        if (!entity.IsValid()) return;

        // Destroy all children first (recursively)
        if (entity.HasComponent<Children>()) {
            auto children = entity.GetComponent<Children>().Value;
            for (auto child : children) {
                DestroyEntity(child);
            }
        }

        // Remove from parent's children list
        if (entity.HasComponent<Parent>()) {
            Entity parent = entity.GetComponent<Parent>().Value;
            if (parent.IsValid() && parent.HasComponent<Children>()) {
                auto& siblings = parent.GetComponent<Children>().Value;
                siblings.erase(std::remove(siblings.begin(), siblings.end(), entity), siblings.end());
            }
        }
        else
            RemoveFromRoots(entity);

        // Finally destroy the entity itself
        std::string name = entity.GetName();
        m_Registry.destroy(entity);
        LH_CORE_TRACE("Destroyed entity: {0}", name);
        IncrementHierarchyVersion();
    }

    Entity Scene::DuplicateEntity(Entity original, bool skipParentAddition)
    {
        if (!original.IsValid()) return {};

        std::string newName = GenerateUniqueName(original);
        Entity duplicate = CreateEntity(newName);

        // Copy all components except hierarchy-related ones
        //original.CopyComponentIfExists<Tag>(duplicate);
        original.CopyComponentIfExists<Transform>(duplicate);
        original.CopyComponentIfExists<Camera>(duplicate);
        original.CopyComponentIfExists<MeshRenderer>(duplicate);
        original.CopyComponentIfExists<Animation>(duplicate);
        original.CopyComponentIfExists<DirectionalLight>(duplicate);
        original.CopyComponentIfExists<PointLight>(duplicate);

        // Handle parent relationship if not skipped
        if (!skipParentAddition && original.HasComponent<Parent>()) {
            Entity parent = original.GetComponent<Parent>().Value;

            if (parent.IsValid()) {
                // Add duplicate to parent's children list
                if (parent.HasComponent<Children>()) {
                    auto& parentChildren = parent.GetComponent<Children>().Value;
                    parentChildren.push_back(duplicate);
                }
                else {
                    auto& parentChildren = parent.AddComponent<Children>().Value;
                    parentChildren.push_back(duplicate);
                }

                // Set duplicate's parent
                duplicate.AddComponent<Parent>().Value = parent;
            }
        }
        else
            m_RootEntities.push_back(duplicate);

        // Recursively duplicate children
        if (original.HasComponent<Children>()) {
            auto& originalChildren = original.GetComponent<Children>().Value;
            auto& duplicateChildren = duplicate.AddComponent<Children>().Value;

            for (Entity child : originalChildren) {
                // Pass 'true' to skip adding the child duplicate to the original parent's children
                Entity duplicatedChild = DuplicateEntity(child, true);
                duplicatedChild.AddOrReplaceComponent<Parent>().Value = duplicate;
                duplicateChildren.push_back(duplicatedChild);
            }
        }

        LH_CORE_TRACE("Duplicated {0} '{1}'",
            original.HasComponent<Children>() ? "hierarchy" : "entity",
            original.GetName());
        return duplicate;
    }

    void Scene::ReorderEntity(Entity entity, Entity target, bool after)
    {
        if (entity == target) return;

        // 1. Ensure they share the same parent (reparent if necessary)
        if (entity.GetParent() != target.GetParent())
        {
            entity.SetParent(target.GetParent());
        }

        // 2. Get the list to modify
        std::vector<Entity>* list = nullptr;
        if (entity.HasParent())
        {
            list = &entity.GetParent().GetComponent<Children>().Value;
        }
        else
        {
            list = &m_RootEntities;
        }

        // 3. Move in list
        auto itEntity = std::find(list->begin(), list->end(), entity);
        if (itEntity != list->end())
        {
            list->erase(itEntity);
        }

        auto itTarget = std::find(list->begin(), list->end(), target);
        if (itTarget != list->end())
        {
            if (after) itTarget++;
            list->insert(itTarget, entity);
        }
        else
        {
            list->push_back(entity); // Fallback
        }
    }

    Entity Scene::FindEntityByUUID(UUID uuid) const
    {
        auto view = m_Registry.view<ID>();
        for (auto e : view) {
            if (view.get<ID>(e).Value == uuid)
                return Entity{ e, const_cast<Scene*>(this) };
        }
        return {};
    }

    void Scene::AddToRoots(Entity entity) {
        m_RootEntities.push_back(entity);
    }

    void Scene::RemoveFromRoots(Entity entity) {
        m_RootEntities.erase(std::remove(m_RootEntities.begin(), m_RootEntities.end(), entity), m_RootEntities.end());
    }

    std::string Scene::GenerateUniqueName(Entity entity)
    {
        if (!entity.IsValid()) return "";

        // Get the parent of the entity
        Entity parent;
        if (entity.HasComponent<Parent>()) {
            parent = entity.GetComponent<Parent>().Value;
        }

        // Get all siblings (children of the parent or root entities)
        std::vector<Entity> siblings;
        if (parent.IsValid()) {
            if (parent.HasComponent<Children>()) {
                siblings = parent.GetComponent<Children>().Value;
            }
        }
        else {
            siblings = m_RootEntities;
        }

        // Extract base name and original number from the entity's name
        std::string name = entity.GetName();
        std::string base = name;
        int originalNumber = 0;

        std::regex pattern(R"(^(.*?)\s\((\d+)\)$)");
        std::smatch matches;
        if (std::regex_match(name, matches, pattern)) {
            base = matches[1].str();
            originalNumber = std::stoi(matches[2].str());
        }

        // Collect all numbers from siblings' names matching the base
        std::vector<int> numbers;
        numbers.push_back(originalNumber); // Include the entity's own number

        std::regex siblingPattern(R"(^(.*?)\s\((\d+)\)$)");
        for (Entity sibling : siblings) {
            // Skip the entity itself
            if (sibling == entity)
                continue;

            std::string siblingName = sibling.GetName();

            if (siblingName == base) {
                numbers.push_back(0);
            }
            else {
                std::smatch siblingMatches;
                if (std::regex_match(siblingName, siblingMatches, siblingPattern)) {
                    std::string siblingBase = siblingMatches[1].str();
                    if (siblingBase == base) {
                        int num = std::stoi(siblingMatches[2].str());
                        numbers.push_back(num);
                    }
                }
            }
        }

        // Determine the new number
        int maxNumber = numbers.empty() ? -1 : *std::max_element(numbers.begin(), numbers.end());
        int newNumber = maxNumber + 1;

        // Generate the new name
        return base + " (" + std::to_string(newNumber) + ")";
    }

    void Scene::HoldAsset(UUID uuid, std::shared_ptr<Asset> asset)
    {
        m_HeldAssets[uuid] = asset;

        // If it's a material, also hold its referenced textures
        if (asset->GetType() == AssetType::Material)
        {
            auto mat = std::static_pointer_cast<Material>(asset);
            for (const auto& map : mat->GetTextures())
            {
                if (map.Uuid.IsValid())
                {
                    auto tex = AssetManager::GetAsset<Texture>(map.Uuid);
                    if (tex)
                        m_HeldAssets[map.Uuid] = tex;
                }
            }
        }
    }

    void Scene::ReleaseAsset(UUID uuid)
    {
        m_HeldAssets.erase(uuid);
    }

    void Scene::ReleaseAllAssets()
    {
        m_HeldAssets.clear();
    }
}
