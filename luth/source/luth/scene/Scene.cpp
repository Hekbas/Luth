#include "luthpch.h"
#include "luth/scene/Scene.h"
#include "luth/scene/Components.h"

namespace Luth
{
    Scene::Scene()
    {
        LH_CORE_INFO("Created new scene");
    }

    Scene::~Scene()
    {
        m_Registry.clear();
        LH_CORE_INFO("Destroyed scene");
    }

    Entity Scene::CreateEntity(const std::string& name)
    {
        Entity entity = { m_Registry.create(), this };
        entity.AddComponent<ID>();
        entity.AddComponent<Tag>(name);
        entity.AddComponent<Transform>();
        LH_CORE_TRACE("Created entity: {0}", name);
        return entity;
    }

    void Scene::DestroyEntity(Entity entity)
    {
        if (!entity.IsValid()) return;

        // Destroy all children first (recursively)
        if (entity.HasComponent<Children>()) {
            auto children = entity.GetComponent<Children>().m_Children;
            for (auto child : children) {
                DestroyEntity(child);
            }
        }

        // Remove from parent's children list
        if (entity.HasComponent<Parent>()) {
            Entity parent = entity.GetComponent<Parent>().m_Parent;
            if (parent.IsValid() && parent.HasComponent<Children>()) {
                auto& siblings = parent.GetComponent<Children>().m_Children;
                siblings.erase(std::remove(siblings.begin(), siblings.end(), entity), siblings.end());
            }
        }

        // Finally destroy the entity itself
        m_Registry.destroy(entity);
        LH_CORE_TRACE("Destroyed entity: {0}", entity.GetName());
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
        // Add other component copies here...

        // Handle parent relationship if not skipped
        if (!skipParentAddition && original.HasComponent<Parent>()) {
            Entity parent = original.GetComponent<Parent>().m_Parent;

            if (parent.IsValid()) {
                // Add duplicate to parent's children list
                if (parent.HasComponent<Children>()) {
                    auto& parentChildren = parent.GetComponent<Children>().m_Children;
                    parentChildren.push_back(duplicate);
                }
                else {
                    auto& parentChildren = parent.AddComponent<Children>().m_Children;
                    parentChildren.push_back(duplicate);
                }

                // Set duplicate's parent
                duplicate.AddComponent<Parent>().m_Parent = parent;
            }
        }

        // Recursively duplicate children
        if (original.HasComponent<Children>()) {
            auto& originalChildren = original.GetComponent<Children>().m_Children;
            auto& duplicateChildren = duplicate.AddComponent<Children>().m_Children;

            for (Entity child : originalChildren) {
                // Pass 'true' to skip adding the child duplicate to the original parent's children
                Entity duplicatedChild = DuplicateEntity(child, true);
                duplicatedChild.AddOrReplaceComponent<Parent>().m_Parent = duplicate;
                duplicateChildren.push_back(duplicatedChild);
            }
        }

        LH_CORE_TRACE("Duplicated {0} '{1}'",
            original.HasComponent<Children>() ? "hierarchy" : "entity",
            original.GetName());
        return duplicate;
    }

    std::string Scene::GenerateUniqueName(Entity entity)
    {
        if (!entity.IsValid()) return "";

        // Get the parent of the entity
        Entity parent;
        if (entity.HasComponent<Parent>()) {
            parent = entity.GetComponent<Parent>().m_Parent;
        }

        // Get all siblings (children of the parent or root entities)
        std::vector<Entity> siblings;
        if (parent.IsValid()) {
            if (parent.HasComponent<Children>()) {
                siblings = parent.GetComponent<Children>().m_Children;
            }
        }
        else {
            // Assuming GetRootEntities() retrieves all root entities
            //siblings = GetRootEntities();
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
}
