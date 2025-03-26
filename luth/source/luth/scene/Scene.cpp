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
        LH_CORE_INFO("Created entity: {0}", name);
        return entity;
    }

    void Scene::DestroyEntity(Entity entity)
    {
        if (!entity) return;

        // Clear parent reference in children
        if (entity.HasComponent<Children>()) {
            for (auto child : entity.GetComponent<Children>().m_Children) {
                if (child.HasComponent<Parent>()) {
                    child.RemoveComponent<Parent>();
                }
            }
        }

        // Remove from parent's children list
        if (entity.HasComponent<Parent>()) {
            Entity parent = entity.GetComponent<Parent>().m_Parent;
            if (parent && parent.HasComponent<Children>()) {
                auto& siblings = parent.GetComponent<Children>().m_Children;
                siblings.erase(std::remove(siblings.begin(), siblings.end(), entity), siblings.end());
            }
        }

        m_Registry.destroy(entity);
        LH_CORE_INFO("Destroyed entity: {0}", entity.GetName());
    }

    Entity Scene::DuplicateEntity(Entity original)
    {
        if (!original.IsValid()) return {};

        std::string newName = GenerateUniqueName(original.GetName());
        Entity duplicate = CreateEntity(newName);

        // Copy components
        /*if (original.HasComponent<Transform>()) {
            duplicate.AddComponent<Transform>(
                original.GetComponent<Transform>()
            );
        }*/

        LH_CORE_INFO("Duplicated entity {0} to {1}", original.GetName(), newName);
        return duplicate;
    }

    std::string Scene::GenerateUniqueName(const std::string& originalName)
    {
        std::string baseName = originalName;
        int currentNumber = 0;

        // Check if original name ends with "(X)" pattern
        size_t openParen = originalName.rfind(" (");
        size_t closeParen = originalName.rfind(')');

        if (openParen != std::string::npos &&
            closeParen == originalName.size() - 1 &&
            closeParen > openParen + 2) {

            std::string numberStr = originalName.substr(openParen + 2,
                closeParen - openParen - 2);
            try {
                currentNumber = std::stoi(numberStr);
                baseName = originalName.substr(0, openParen);
            }
            catch (...) {
                // Not a valid number, keep original name
            }
        }

        // Find existing numbers for this base name
        std::vector<int> existingNumbers;
        auto view = m_Registry.view<Tag>();

        for (auto entity : view) {
            Entity e{ entity, this };
            std::string name = e.GetName();

            if (name.find(baseName) != 0) continue; // Doesn't start with base name

            size_t pos = baseName.size();
            if (name.size() > pos &&
                name[pos] == ' ' &&
                name[pos + 1] == '(' &&
                name.back() == ')') {

                size_t numStart = pos + 2;
                size_t numEnd = name.size() - 1;
                if (numStart >= numEnd) continue;

                try {
                    int num = std::stoi(name.substr(numStart, numEnd - numStart));
                    existingNumbers.push_back(num);
                }
                catch (...) {
                    // Not a valid number, skip
                }
            }
        }

        // Calculate next available number
        int nextNumber = currentNumber + 1;
        if (!existingNumbers.empty()) {
            auto maxIt = std::max_element(existingNumbers.begin(), existingNumbers.end());
            nextNumber = std::max(nextNumber, *maxIt + 1);
        }

        // Generate unique name
        std::string newName;
        int attempt = nextNumber;
        while (true) {
            newName = fmt::format("{0} ({1})", baseName, attempt);
            bool exists = false;

            for (auto entity : view) {
                Entity e{ entity, this };
                if (e.GetName() == newName) {
                    exists = true;
                    break;
                }
            }

            if (!exists) break;
            attempt++;
        }

        return newName;
    }
}
