#include "luthpch.h"
#include "luth/scene/Scene.h"
#include "luth/scene/Components.h"
#include "luth/renderer/material/Material.h"
#include "luth/renderer/resources/Texture.h"
#include "luth/renderer/resources/Model.h"
#include "luth/resources/AssetManager.h"

#include <regex>

namespace Luth
{
    using namespace Component;

    Scene::Scene()
    {
        LH_CORE_INFO("Created new scene");
    }

    Scene::~Scene()
    {
        // Iterate-destroy first so EnTT destroy signals fire and any system listening on component
        // lifecycle (PhysicsSystem, future Audio sources, etc.) can release backing resources before
        // the registry tears down. The trailing m_Registry.clear() then resets sparse-set buckets.
        ClearPreservingAssets();
        m_Registry.clear();
        LH_CORE_INFO("Destroyed scene");
    }

    void Scene::Clear()
    {
        ReleaseAllAssets();
        ClearPreservingAssets();
    }

    void Scene::ClearPreservingAssets()
    {
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

    Entity Scene::InstantiateModel(const std::shared_ptr<Model>& model, Entity parent)
    {
        if (!model) return {};
        const UUID modelUUID = model->Handle;

        // Attach a MeshRenderer for `meshIdx` to `e`, wiring the material + kicking its async load.
        auto attachMesh = [&](Entity e, u32 meshIdx) {
            auto& mr = e.AddComponent<MeshRenderer>();
            mr.ModelUUID = modelUUID;
            mr.MeshIndex = meshIdx;
            mr.isSkinned = model->IsSkinned();
            const auto& info = model->GetCachedModelInfo();
            const auto& materials = model->GetMaterials();
            u32 materialIdx = (meshIdx < info.Meshes.size()) ? info.Meshes[meshIdx].MaterialIndex : 0;
            if (materialIdx < materials.size() && materials[materialIdx].IsValid()) {
                mr.MaterialUUID = materials[materialIdx];
                AssetManager::LoadAsync(mr.MaterialUUID);
            }
        };

        if (model->HasNodeTree()) {
            // Static: faithful node hierarchy. Nodes are topological, so a parent entity always exists
            // before its children reference it. Transforms decompose to the Transform component's
            // Euler-degrees convention (matches AnimationSystem's DecomposeTransform write-back).
            const auto& nodes   = model->GetNodes();
            const auto& cameras = model->GetCameras();
            const auto& lights  = model->GetLights();
            if (nodes.empty()) return Entity{};
            std::vector<Entity> nodeEntities(nodes.size());

            // Skip a content-less single-child scene root (common FBX artifact) so single-mesh imports are one entity.
            i32 skipRoot = -1;
            if (nodes[0].MeshIndices.empty() && nodes[0].CameraIndex < 0 && nodes[0].LightIndex < 0) {
                i32 rootChildren = 0;
                for (const auto& n : nodes) if (n.ParentIndex == 0) ++rootChildren;
                if (rootChildren == 1) skipRoot = 0;
            }

            Entity importRoot{};
            for (size_t i = 0; i < nodes.size(); ++i) {
                if ((i32)i == skipRoot) { nodeEntities[i] = Entity{}; continue; }
                const auto& n = nodes[i];
                Entity e = CreateEntity(n.Name.empty() ? "Node" : n.Name);
                nodeEntities[i] = e;

                auto& t = e.GetComponent<Transform>();
                if (n.ParentIndex == skipRoot) {
                    // Fold the skipped root's local transform in so the child keeps its world pose.
                    const auto& r = nodes[skipRoot];
                    Mat4 rootM  = Math::Translate(Mat4(1.0f), r.Translation) * Math::ToMat4(r.Rotation) * Math::Scale(Mat4(1.0f), r.Scale);
                    Mat4 childM = Math::Translate(Mat4(1.0f), n.Translation) * Math::ToMat4(n.Rotation) * Math::Scale(Mat4(1.0f), n.Scale);
                    Quat q;
                    DecomposeTransform(rootM * childM, t.Position, q, t.Scale);
                    t.Rotation = Math::Degrees(Math::EulerAngles(q));
                } else {
                    t.Position = n.Translation;
                    t.Rotation = Math::Degrees(Math::EulerAngles(n.Rotation));
                    t.Scale    = n.Scale;
                }
                t.IsDirty = true;

                if (n.ParentIndex >= 0 && n.ParentIndex != skipRoot) e.SetParent(nodeEntities[n.ParentIndex]);
                else {
                    if (parent && parent.IsValid()) e.SetParent(parent);
                    importRoot = e;
                }

                // One mesh rides on the node entity; extra meshes become identity-local children.
                if (n.MeshIndices.size() == 1) {
                    attachMesh(e, n.MeshIndices[0]);
                } else {
                    for (u32 mi : n.MeshIndices) {
                        Entity child = CreateEntity(model->GetCachedModelInfo().Meshes[mi].Name);
                        child.SetParent(e);
                        attachMesh(child, mi);
                    }
                }

                if (n.CameraIndex >= 0 && n.CameraIndex < (i32)cameras.size()) {
                    const auto& mc = cameras[n.CameraIndex];
                    auto& cam = e.AddComponent<Camera>();
                    cam.VerticalFOV = mc.FovYDeg;
                    cam.NearClip    = mc.NearClip;
                    cam.FarClip     = mc.FarClip;
                    cam.AspectRatio = mc.Aspect;
                    cam.IsDirty     = true;
                }

                if (n.LightIndex >= 0 && n.LightIndex < (i32)lights.size()) {
                    const auto& ml = lights[n.LightIndex];
                    if (ml.Type == 0) {
                        auto& dl = e.AddComponent<DirectionalLight>();
                        dl.Color = ml.Color;
                        dl.Intensity = ml.Intensity;
                    } else if (ml.Type == 2) {
                        auto& sl = e.AddComponent<SpotLight>();
                        sl.Color = ml.Color;
                        sl.Intensity = ml.Intensity;
                        sl.Range = ml.Range;
                        sl.InnerConeAngleDeg = ml.InnerConeAngleDeg;
                        sl.OuterConeAngleDeg = ml.OuterConeAngleDeg;
                    } else {
                        auto& pl = e.AddComponent<PointLight>();
                        pl.Color = ml.Color;
                        pl.Intensity = ml.Intensity;
                        pl.Range = ml.Range;
                    }
                }
            }

            return importRoot;
        }

        // Skinned / legacy no-tree: root + flat mesh children + bone-entity hierarchy (prior behavior).
        Entity root = CreateEntity(model->GetName());
        if (parent && parent.IsValid()) root.SetParent(parent);
        if (model->IsSkinned()) root.AddComponent<Animation>(modelUUID);

        const auto& meshes = model->GetMeshes();
        for (size_t i = 0; i < meshes.size(); i++) {
            Entity child = CreateEntity(model->GetCachedModelInfo().Meshes[i].Name);
            child.SetParent(root);
            attachMesh(child, (u32)i);
        }

        if (model->IsSkinned() && !model->GetSkeleton().IsEmpty()) {
            const auto& skeleton = model->GetSkeleton();
            u32 boneCount = skeleton.BoneCount();
            std::vector<Entity> boneEntities(boneCount);
            for (u32 i = 0; i < boneCount; i++) {
                Entity boneEntity = CreateEntity(skeleton.Bones[i].Name);
                boneEntities[i] = boneEntity;
                i32 parentIdx = skeleton.Bones[i].ParentIndex;
                boneEntity.SetParent((parentIdx >= 0 && parentIdx < (i32)boneCount)
                    ? boneEntities[parentIdx] : root);
            }
        }

        return root;
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
        // invariant: CreateEntity always pushes to m_RootEntities. Anything below
        // that re-parents the duplicate (or expects the caller to) MUST RemoveFromRoots
        // first, otherwise the entity appears twice in the hierarchy.
        Entity duplicate = CreateEntity(newName);

        // Copy all components except hierarchy-related ones
        //original.CopyComponentIfExists<Tag>(duplicate);
        original.CopyComponentIfExists<Transform>(duplicate);
        original.CopyComponentIfExists<Camera>(duplicate);
        original.CopyComponentIfExists<MeshRenderer>(duplicate);
        original.CopyComponentIfExists<Animation>(duplicate);
        original.CopyComponentIfExists<DirectionalLight>(duplicate);
        original.CopyComponentIfExists<PointLight>(duplicate);
        original.CopyComponentIfExists<SpotLight>(duplicate);
        original.CopyComponentIfExists<Collider>(duplicate);
        original.CopyComponentIfExists<RigidBody>(duplicate);
        // PhysicsBodyRuntime is intentionally not copied — PhysicsSystem's on_construct signal will
        // create a fresh body for the duplicate when its (Collider + RigidBody) pair completes.

        // Resolve final parent. Recursive-children case (skipParentAddition)
        // hands off to the caller, which assigns Parent on the next line.
        Entity newParent = {};
        if (!skipParentAddition && original.HasComponent<Parent>()) {
            Entity p = original.GetComponent<Parent>().Value;
            if (p.IsValid()) newParent = p;
        }

        // Strip from roots before any reparenting so the entity ends up in
        // exactly one place (root list OR parent's Children, never both).
        if (skipParentAddition || newParent.IsValid())
            RemoveFromRoots(duplicate);

        if (newParent.IsValid()) {
            if (newParent.HasComponent<Children>())
                newParent.GetComponent<Children>().Value.push_back(duplicate);
            else
                newParent.AddComponent<Children>().Value.push_back(duplicate);
            duplicate.AddComponent<Parent>().Value = newParent;
        }

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
