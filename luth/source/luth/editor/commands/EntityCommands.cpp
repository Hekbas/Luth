#include "luthpch.h"
#include "luth/editor/commands/EntityCommands.h"
#include "luth/scene/Scene.h"
#include "luth/scene/Components.h"
#include "luth/editor/EditorSelection.h"

namespace Luth
{
    using namespace Component;
    using json = nlohmann::json;

    // ═══════════════════════════════════════════════════════════════════════════
    //  CommandUtil — serialization helpers
    // ═══════════════════════════════════════════════════════════════════════════

    namespace CommandUtil
    {
        static json Vec3ToJson(const glm::vec3& v)
        {
            return { v.x, v.y, v.z };
        }

        static glm::vec3 Vec3FromJson(const json& j, const glm::vec3& fallback = glm::vec3(0.0f))
        {
            if (!j.is_array() || j.size() < 3) return fallback;
            return { j[0].get<float>(), j[1].get<float>(), j[2].get<float>() };
        }

        json SerializeEntity(Entity entity)
        {
            json j;
            j["uuid"]   = entity.GetComponent<ID>().Value.ToString();
            j["tag"]    = entity.GetComponent<Tag>().Value;
            j["active"] = entity.IsActive();

            if (entity.HasParent()) {
                Entity parent = entity.GetParent();
                j["parent"] = parent.GetComponent<ID>().Value.ToString();
            } else {
                j["parent"] = "";
            }

            {
                auto& t = entity.GetComponent<Transform>();
                json tj;
                tj["position"] = Vec3ToJson(t.Position);
                tj["rotation"] = Vec3ToJson(t.Rotation);
                tj["scale"]    = Vec3ToJson(t.Scale);
                j["transform"] = tj;
            }

            if (entity.HasComponent<Camera>()) {
                auto& c = entity.GetComponent<Camera>();
                json cj;
                cj["projection"]       = static_cast<int>(c.Projection);
                cj["verticalFOV"]      = c.VerticalFOV;
                cj["nearClip"]         = c.NearClip;
                cj["farClip"]          = c.FarClip;
                cj["orthographicSize"] = c.OrthographicSize;
                cj["orthographicNear"] = c.OrthographicNear;
                cj["orthographicFar"]  = c.OrthographicFar;
                j["camera"] = cj;
            }

            if (entity.HasComponent<MeshRenderer>()) {
                auto& mr = entity.GetComponent<MeshRenderer>();
                json mj;
                mj["modelUUID"]    = mr.ModelUUID.ToString();
                mj["meshIndex"]    = mr.MeshIndex;
                mj["materialUUID"] = mr.MaterialUUID.ToString();
                mj["isSkinned"]    = mr.isSkinned;
                j["meshRenderer"]  = mj;
            }

            if (entity.HasComponent<Animation>()) {
                auto& a = entity.GetComponent<Animation>();
                json aj;
                aj["modelUUID"]      = a.ModelUUID.ToString();
                aj["animationIndex"] = a.AnimationIndex;
                aj["speed"]          = a.Speed;
                aj["loopMode"]       = static_cast<int>(a.LoopMode);
                aj["playing"]        = a.Playing;
                j["animation"]       = aj;
            }

            if (entity.HasComponent<AnimationController>()) {
                auto& ctrl = entity.GetComponent<AnimationController>();
                json cj;
                cj["currentClipIndex"]          = ctrl.CurrentClipIndex;
                cj["applyRootMotion"]           = ctrl.ApplyRootMotion;
                cj["defaultTransitionDuration"] = ctrl.DefaultTransitionDuration;

                json layersJson = json::array();
                for (const auto& layer : ctrl.Layers) {
                    json lj;
                    lj["clipIndex"] = layer.ClipIndex;
                    lj["speed"]     = layer.Speed;
                    lj["weight"]    = layer.Weight;
                    lj["loop"]      = layer.Loop;
                    if (!layer.BoneMask.empty()) {
                        json maskJson = json::array();
                        for (u32 i = 0; i < (u32)layer.BoneMask.size(); i++)
                            if (layer.BoneMask[i]) maskJson.push_back(i);
                        lj["boneMask"] = maskJson;
                    }
                    layersJson.push_back(lj);
                }
                cj["layers"] = layersJson;
                j["animationController"] = cj;
            }

            if (entity.HasComponent<BoneAttachment>()) {
                auto& ba = entity.GetComponent<BoneAttachment>();
                json baj;
                if (ba.TargetEntity && ba.TargetEntity.IsValid()) {
                    baj["targetUUID"] = ba.TargetEntity.GetComponent<ID>().Value.ToString();
                }
                baj["boneName"]      = ba.BoneName;
                baj["localOffset"]   = Vec3ToJson(ba.LocalOffset);
                baj["localRotation"] = Vec3ToJson(ba.LocalRotation);
                j["boneAttachment"]  = baj;
            }

            if (entity.HasComponent<DirectionalLight>()) {
                auto& dl = entity.GetComponent<DirectionalLight>();
                json dj;
                dj["color"]           = Vec3ToJson(dl.Color);
                dj["intensity"]       = dl.Intensity;
                dj["castShadows"]     = dl.CastShadows;
                dj["shadowOrthoSize"] = dl.ShadowOrthoSize;
                dj["shadowDistance"]   = dl.ShadowDistance;
                dj["splitLambda"]         = dl.SplitLambda;
                dj["stabilizeCascades"]   = dl.StabilizeCascades;
                dj["shadowBias"]          = { dl.ShadowBias[0], dl.ShadowBias[1], dl.ShadowBias[2], dl.ShadowBias[3] };
                dj["shadowNormalBias"]    = { dl.ShadowNormalBias[0], dl.ShadowNormalBias[1], dl.ShadowNormalBias[2], dl.ShadowNormalBias[3] };
                dj["cascadeBlendWidth"]   = dl.CascadeBlendWidth;
                dj["debugVisualizeCascades"] = dl.DebugVisualizeCascades;
                j["directionalLight"] = dj;
            }

            if (entity.HasComponent<PointLight>()) {
                auto& pl = entity.GetComponent<PointLight>();
                json pj;
                pj["color"]     = Vec3ToJson(pl.Color);
                pj["intensity"] = pl.Intensity;
                pj["range"]     = pl.Range;
                j["pointLight"] = pj;
            }

            return j;
        }

        static void CollectDFS(Entity entity, std::vector<Entity>& out)
        {
            out.push_back(entity);
            for (Entity child : entity.GetChildren())
                CollectDFS(child, out);
        }

        json SerializeEntitySubtree(Entity root)
        {
            std::vector<Entity> entities;
            CollectDFS(root, entities);

            json arr = json::array();
            for (Entity e : entities)
                arr.push_back(SerializeEntity(e));
            return arr;
        }

        void DeserializeEntitySubtree(Scene& scene, const json& snapshot)
        {
            if (!snapshot.is_array() || snapshot.empty()) return;

            std::unordered_map<std::string, Entity> uuidToEntity;
            std::vector<std::pair<Entity, std::string>> parentLinks;
            std::vector<std::pair<Entity, std::string>> attachmentLinks;

            for (const auto& ej : snapshot)
            {
                std::string tag = ej.value("tag", "Entity");
                Entity entity = scene.CreateEntity(tag);

                // Overwrite auto-generated UUID
                std::string uuidStr = ej.value("uuid", "");
                if (!uuidStr.empty())
                    entity.GetComponent<ID>().Value = UUID::FromString(uuidStr);

                entity.SetActive(ej.value("active", true));

                // Transform
                if (ej.contains("transform")) {
                    auto& t = entity.GetComponent<Transform>();
                    const auto& tj = ej["transform"];
                    t.Position = Vec3FromJson(tj.value("position", json::array()), {0,0,0});
                    t.Rotation = Vec3FromJson(tj.value("rotation", json::array()), {0,0,0});
                    t.Scale    = Vec3FromJson(tj.value("scale",    json::array()), {1,1,1});
                    t.IsDirty = true;
                }

                // Camera
                if (ej.contains("camera")) {
                    const auto& cj = ej["camera"];
                    auto& c = entity.AddComponent<Camera>();
                    c.Projection       = static_cast<Camera::ProjectionType>(cj.value("projection", 0));
                    c.VerticalFOV      = cj.value("verticalFOV", 45.0f);
                    c.NearClip         = cj.value("nearClip", 0.01f);
                    c.FarClip          = cj.value("farClip", 1000.0f);
                    c.OrthographicSize = cj.value("orthographicSize", 10.0f);
                    c.OrthographicNear = cj.value("orthographicNear", -1.0f);
                    c.OrthographicFar  = cj.value("orthographicFar", 1.0f);
                    c.IsDirty = true;
                }

                // MeshRenderer
                if (ej.contains("meshRenderer")) {
                    const auto& mj = ej["meshRenderer"];
                    auto& mr = entity.AddComponent<MeshRenderer>();
                    mr.ModelUUID    = UUID::FromString(mj.value("modelUUID", ""));
                    mr.MeshIndex    = mj.value("meshIndex", 0u);
                    mr.MaterialUUID = UUID::FromString(mj.value("materialUUID", ""));
                    mr.isSkinned    = mj.value("isSkinned", false);
                }

                // Animation
                if (ej.contains("animation")) {
                    const auto& aj = ej["animation"];
                    auto& a = entity.AddComponent<Animation>();
                    a.ModelUUID      = UUID::FromString(aj.value("modelUUID", ""));
                    a.AnimationIndex = aj.value("animationIndex", 0);
                    a.Speed          = aj.value("speed", 1.0f);
                    if (aj.contains("loopMode"))
                        a.LoopMode = static_cast<AnimationLoopMode>(aj.value("loopMode", 1));
                    else
                        a.LoopMode = aj.value("loop", true) ? AnimationLoopMode::One : AnimationLoopMode::Off;
                    a.Playing = aj.value("playing", false);
                }

                // AnimationController
                if (ej.contains("animationController")) {
                    const auto& cj = ej["animationController"];
                    auto& ctrl = entity.AddComponent<AnimationController>();
                    ctrl.CurrentClipIndex          = cj.value("currentClipIndex", 0);
                    ctrl.ApplyRootMotion           = cj.value("applyRootMotion", false);
                    ctrl.DefaultTransitionDuration = cj.value("defaultTransitionDuration", 0.2f);

                    if (cj.contains("layers")) {
                        for (const auto& lj : cj["layers"]) {
                            BlendLayer layer;
                            layer.ClipIndex = lj.value("clipIndex", -1);
                            layer.Speed     = lj.value("speed", 1.0f);
                            layer.Weight    = lj.value("weight", 1.0f);
                            layer.Loop      = lj.value("loop", true);
                            if (lj.contains("boneMask")) {
                                const auto& maskJson = lj["boneMask"];
                                if (!maskJson.empty()) {
                                    u32 maxIdx = 0;
                                    for (const auto& idx : maskJson)
                                        maxIdx = std::max(maxIdx, idx.get<u32>());
                                    layer.BoneMask.resize(maxIdx + 1, false);
                                    for (const auto& idx : maskJson)
                                        layer.BoneMask[idx.get<u32>()] = true;
                                }
                            }
                            ctrl.Layers.push_back(layer);
                        }
                    }
                }

                // BoneAttachment
                if (ej.contains("boneAttachment")) {
                    const auto& baj = ej["boneAttachment"];
                    auto& ba = entity.AddComponent<BoneAttachment>();
                    ba.BoneName      = baj.value("boneName", "");
                    ba.LocalOffset   = Vec3FromJson(baj.value("localOffset",   json::array()), {0,0,0});
                    ba.LocalRotation = Vec3FromJson(baj.value("localRotation", json::array()), {0,0,0});
                    if (baj.contains("targetUUID") && !baj["targetUUID"].get<std::string>().empty())
                        attachmentLinks.push_back({ entity, baj["targetUUID"].get<std::string>() });
                }

                // DirectionalLight
                if (ej.contains("directionalLight")) {
                    const auto& dj = ej["directionalLight"];
                    auto& dl = entity.AddComponent<DirectionalLight>();
                    dl.Color         = Vec3FromJson(dj.value("color", json::array()), {1,1,1});
                    dl.Intensity     = dj.value("intensity", 1.0f);
                    dl.CastShadows   = dj.value("castShadows", true);
                    dl.ShadowOrthoSize = dj.value("shadowOrthoSize", 200.0f);
                    dl.ShadowDistance = dj.value("shadowDistance", 200.0f);
                    dl.SplitLambda        = dj.value("splitLambda", 0.5f);
                    dl.StabilizeCascades  = dj.value("stabilizeCascades", true);

                    if (dj.contains("shadowBias") && dj["shadowBias"].is_array()) {
                        const auto& arr = dj["shadowBias"];
                        for (u32 i = 0; i < 4; ++i)
                            dl.ShadowBias[i] = (i < arr.size()) ? arr[i].get<float>() : dl.ShadowBias[i];
                    }
                    else {
                        float legacy = dj.value("shadowBias", 0.005f);
                        for (u32 i = 0; i < 4; ++i) dl.ShadowBias[i] = legacy;
                    }
                    if (dj.contains("shadowNormalBias") && dj["shadowNormalBias"].is_array()) {
                        const auto& arr = dj["shadowNormalBias"];
                        for (u32 i = 0; i < 4; ++i)
                            dl.ShadowNormalBias[i] = (i < arr.size()) ? arr[i].get<float>() : dl.ShadowNormalBias[i];
                    }
                    dl.CascadeBlendWidth       = dj.value("cascadeBlendWidth", 0.2f);
                    dl.DebugVisualizeCascades   = dj.value("debugVisualizeCascades", false);
                }

                // PointLight
                if (ej.contains("pointLight")) {
                    const auto& pj = ej["pointLight"];
                    auto& pl = entity.AddComponent<PointLight>();
                    pl.Color     = Vec3FromJson(pj.value("color", json::array()), {1,1,1});
                    pl.Intensity = pj.value("intensity", 1.0f);
                    pl.Range     = pj.value("range", 350.0f);
                }

                uuidToEntity[ej.value("uuid", "")] = entity;

                // Collect parent link
                std::string parentStr = ej.value("parent", "");
                if (!parentStr.empty())
                    parentLinks.push_back({ entity, parentStr });
            }

            // Pass 2: Restore hierarchy
            for (auto& [child, parentUUIDStr] : parentLinks) {
                auto it = uuidToEntity.find(parentUUIDStr);
                if (it != uuidToEntity.end() && it->second.IsValid()) {
                    scene.RemoveFromRoots(child);
                    child.SetParent(it->second);
                } else {
                    Entity parent = scene.FindEntityByUUID(UUID::FromString(parentUUIDStr));
                    if (parent.IsValid()) {
                        scene.RemoveFromRoots(child);
                        child.SetParent(parent);
                    }
                }
            }

            // Pass 3: Resolve attachment targets
            for (auto& [entity, targetUUIDStr] : attachmentLinks) {
                auto it = uuidToEntity.find(targetUUIDStr);
                if (it != uuidToEntity.end() && it->second.IsValid()) {
                    entity.GetComponent<BoneAttachment>().TargetEntity = it->second;
                } else {
                    Entity target = scene.FindEntityByUUID(UUID::FromString(targetUUIDStr));
                    if (target.IsValid())
                        entity.GetComponent<BoneAttachment>().TargetEntity = target;
                }
            }
        }

        i32 GetSiblingIndex(Entity entity)
        {
            Scene* scene = entity.GetScene();
            if (!scene) return -1;

            const std::vector<Entity>* list = nullptr;
            if (entity.HasParent()) {
                Entity parent = entity.GetParent();
                if (parent.HasComponent<Children>())
                    list = &parent.GetComponent<Children>().Value;
            } else {
                list = &scene->GetRootEntities();
            }

            if (!list) return -1;
            for (i32 i = 0; i < (i32)list->size(); i++) {
                if ((*list)[i] == entity)
                    return i;
            }
            return -1;
        }

        void InsertAtSiblingIndex(Scene& scene, Entity entity, Entity parent, i32 index)
        {
            if (parent.IsValid()) {
                entity.SetParent(parent);

                auto& children = parent.GetComponent<Children>().Value;
                children.erase(std::remove(children.begin(), children.end(), entity), children.end());
                i32 idx = std::min(index, (i32)children.size());
                children.insert(children.begin() + idx, entity);
            }
            else {
                entity.RemoveParent();

                auto& roots = const_cast<std::vector<Entity>&>(scene.GetRootEntities());
                roots.erase(std::remove(roots.begin(), roots.end(), entity), roots.end());
                i32 idx = std::min(index, (i32)roots.size());
                roots.insert(roots.begin() + idx, entity);
            }
        }

    } // namespace CommandUtil

    // ═══════════════════════════════════════════════════════════════════════════
    //  GizmoTransformCommand
    // ═══════════════════════════════════════════════════════════════════════════

    GizmoTransformCommand::GizmoTransformCommand(Scene* scene, entt::entity entity,
        Vec3 oldPos, Vec3 oldRot, Vec3 oldScale,
        Vec3 newPos, Vec3 newRot, Vec3 newScale)
        : m_Scene(scene),
          m_OldPos(oldPos), m_OldRot(oldRot), m_OldScale(oldScale),
          m_NewPos(newPos), m_NewRot(newRot), m_NewScale(newScale)
    {
        Entity e{ entity, scene };
        m_EntityUUID = e.GetComponent<Component::ID>().Value;
    }

    void GizmoTransformCommand::Execute() { Apply(m_NewPos, m_NewRot, m_NewScale); }
    void GizmoTransformCommand::Undo()    { Apply(m_OldPos, m_OldRot, m_OldScale); }
    void GizmoTransformCommand::Redo()    { Apply(m_NewPos, m_NewRot, m_NewScale); }

    void GizmoTransformCommand::Apply(const Vec3& pos, const Vec3& rot, const Vec3& scale)
    {
        Entity e = m_Scene->FindEntityByUUID(m_EntityUUID);
        if (!e.IsValid()) return;
        auto& tc = e.GetComponent<Transform>();
        tc.Position = pos;
        tc.Rotation = rot;
        tc.Scale = scale;
        tc.IsDirty = true;
    }

    // ═══════════════════════════════════════════════════════════════════════════
    //  EntityCreateCommand
    // ═══════════════════════════════════════════════════════════════════════════

    EntityCreateCommand::EntityCreateCommand(Scene* scene, const std::string& name, UUID parentUUID)
        : m_Scene(scene), m_EntityName(name), m_ParentUUID(parentUUID) {}

    void EntityCreateCommand::Execute()
    {
        if (m_FirstExecution)
        {
            Entity entity = m_Scene->CreateEntity(m_EntityName);
            m_CreatedUUID = entity.GetComponent<ID>().Value;

            if (m_ParentUUID.IsValid()) {
                Entity parent = m_Scene->FindEntityByUUID(m_ParentUUID);
                if (parent.IsValid())
                    entity.SetParent(parent);
            }
            m_FirstExecution = false;
        }
        else
        {
            Redo();
        }
    }

    void EntityCreateCommand::Undo()
    {
        Entity entity = m_Scene->FindEntityByUUID(m_CreatedUUID);
        if (entity.IsValid())
            m_Scene->DestroyEntity(entity);
    }

    void EntityCreateCommand::Redo()
    {
        Entity entity = m_Scene->CreateEntity(m_EntityName);
        entity.GetComponent<ID>().Value = m_CreatedUUID;

        if (m_ParentUUID.IsValid()) {
            Entity parent = m_Scene->FindEntityByUUID(m_ParentUUID);
            if (parent.IsValid())
                entity.SetParent(parent);
        }
    }

    Entity EntityCreateCommand::GetCreatedEntity() const
    {
        return m_Scene->FindEntityByUUID(m_CreatedUUID);
    }

    // ═══════════════════════════════════════════════════════════════════════════
    //  EntityDestroyCommand
    // ═══════════════════════════════════════════════════════════════════════════

    EntityDestroyCommand::EntityDestroyCommand(Scene* scene, Entity entity)
        : m_Scene(scene)
    {
        m_EntityUUID = entity.GetComponent<ID>().Value;

        if (entity.HasParent()) {
            Entity parent = entity.GetParent();
            m_ParentUUID = parent.GetComponent<ID>().Value;
        }
        m_SiblingIndex = CommandUtil::GetSiblingIndex(entity);
        m_WasSelected = EditorSelection::IsSelected(entity);
    }

    void EntityDestroyCommand::Execute()
    {
        Entity entity = m_Scene->FindEntityByUUID(m_EntityUUID);
        if (!entity.IsValid()) return;

        m_Snapshot = CommandUtil::SerializeEntitySubtree(entity);
        m_Scene->DestroyEntity(entity);

        if (m_WasSelected)
            EditorSelection::ClearSelection();
    }

    void EntityDestroyCommand::Undo()
    {
        CommandUtil::DeserializeEntitySubtree(*m_Scene, m_Snapshot);

        Entity restored = m_Scene->FindEntityByUUID(m_EntityUUID);
        if (restored.IsValid() && m_SiblingIndex >= 0) {
            Entity parent = m_ParentUUID.IsValid()
                ? m_Scene->FindEntityByUUID(m_ParentUUID)
                : Entity{};
            CommandUtil::InsertAtSiblingIndex(*m_Scene, restored, parent, m_SiblingIndex);
        }

        if (m_WasSelected && restored.IsValid())
            EditorSelection::SelectEntity(restored);
    }

    void EntityDestroyCommand::Redo() { Execute(); }

    // ═══════════════════════════════════════════════════════════════════════════
    //  EntityRenameCommand
    // ═══════════════════════════════════════════════════════════════════════════

    EntityRenameCommand::EntityRenameCommand(Scene* scene, entt::entity entity,
                                             std::string oldName, std::string newName)
        : m_Scene(scene),
          m_OldName(std::move(oldName)), m_NewName(std::move(newName))
    {
        Entity e{ entity, scene };
        m_EntityUUID = e.GetComponent<Component::ID>().Value;
    }

    void EntityRenameCommand::Execute()
    {
        Entity e = m_Scene->FindEntityByUUID(m_EntityUUID);
        if (!e.IsValid()) return;
        e.SetName(m_NewName);
    }

    void EntityRenameCommand::Undo()
    {
        Entity e = m_Scene->FindEntityByUUID(m_EntityUUID);
        if (!e.IsValid()) return;
        e.SetName(m_OldName);
    }

    void EntityRenameCommand::Redo() { Execute(); }

    // ═══════════════════════════════════════════════════════════════════════════
    //  EntityReparentCommand
    // ═══════════════════════════════════════════════════════════════════════════

    EntityReparentCommand::EntityReparentCommand(Scene* scene, Entity entity, Entity newParent)
        : m_Scene(scene)
    {
        m_EntityUUID = entity.GetComponent<ID>().Value;

        if (entity.HasParent())
            m_OldParentUUID = entity.GetParent().GetComponent<ID>().Value;

        if (newParent.IsValid())
            m_NewParentUUID = newParent.GetComponent<ID>().Value;

        m_OldSiblingIndex = CommandUtil::GetSiblingIndex(entity);
    }

    void EntityReparentCommand::Execute()
    {
        Entity entity = m_Scene->FindEntityByUUID(m_EntityUUID);
        if (!entity.IsValid()) return;

        Entity newParent = m_NewParentUUID.IsValid()
            ? m_Scene->FindEntityByUUID(m_NewParentUUID)
            : Entity{};
        entity.SetParent(newParent);
    }

    void EntityReparentCommand::Undo()
    {
        Entity entity = m_Scene->FindEntityByUUID(m_EntityUUID);
        if (!entity.IsValid()) return;

        Entity oldParent = m_OldParentUUID.IsValid()
            ? m_Scene->FindEntityByUUID(m_OldParentUUID)
            : Entity{};
        entity.SetParent(oldParent);

        if (m_OldSiblingIndex >= 0)
            CommandUtil::InsertAtSiblingIndex(*m_Scene, entity, oldParent, m_OldSiblingIndex);
    }

    void EntityReparentCommand::Redo() { Execute(); }

    // ═══════════════════════════════════════════════════════════════════════════
    //  EntityReorderCommand
    // ═══════════════════════════════════════════════════════════════════════════

    EntityReorderCommand::EntityReorderCommand(Scene* scene, Entity entity, Entity target, bool after)
        : m_Scene(scene), m_After(after)
    {
        m_EntityUUID = entity.GetComponent<ID>().Value;
        m_TargetUUID = target.GetComponent<ID>().Value;

        if (entity.HasParent())
            m_OldParentUUID = entity.GetParent().GetComponent<ID>().Value;

        m_OldSiblingIndex = CommandUtil::GetSiblingIndex(entity);
    }

    void EntityReorderCommand::Execute()
    {
        Entity entity = m_Scene->FindEntityByUUID(m_EntityUUID);
        Entity target = m_Scene->FindEntityByUUID(m_TargetUUID);
        if (!entity.IsValid() || !target.IsValid()) return;

        m_Scene->ReorderEntity(entity, target, m_After);
    }

    void EntityReorderCommand::Undo()
    {
        Entity entity = m_Scene->FindEntityByUUID(m_EntityUUID);
        if (!entity.IsValid()) return;

        Entity oldParent = m_OldParentUUID.IsValid()
            ? m_Scene->FindEntityByUUID(m_OldParentUUID)
            : Entity{};

        if (entity.GetParent() != oldParent)
            entity.SetParent(oldParent);

        if (m_OldSiblingIndex >= 0)
            CommandUtil::InsertAtSiblingIndex(*m_Scene, entity, oldParent, m_OldSiblingIndex);
    }

    void EntityReorderCommand::Redo() { Execute(); }

    // ═══════════════════════════════════════════════════════════════════════════
    //  EntityDuplicateCommand
    // ═══════════════════════════════════════════════════════════════════════════

    EntityDuplicateCommand::EntityDuplicateCommand(Scene* scene, Entity original)
        : m_Scene(scene)
    {
        m_OriginalUUID = original.GetComponent<ID>().Value;
    }

    void EntityDuplicateCommand::Execute()
    {
        if (m_FirstExecution)
        {
            Entity original = m_Scene->FindEntityByUUID(m_OriginalUUID);
            if (!original.IsValid()) return;

            Entity duplicate = m_Scene->DuplicateEntity(original);
            m_DuplicateUUID = duplicate.GetComponent<ID>().Value;
            m_DuplicateSnapshot = CommandUtil::SerializeEntitySubtree(duplicate);
            m_FirstExecution = false;
        }
        else
        {
            Redo();
        }
    }

    void EntityDuplicateCommand::Undo()
    {
        Entity duplicate = m_Scene->FindEntityByUUID(m_DuplicateUUID);
        if (duplicate.IsValid())
            m_Scene->DestroyEntity(duplicate);
    }

    void EntityDuplicateCommand::Redo()
    {
        CommandUtil::DeserializeEntitySubtree(*m_Scene, m_DuplicateSnapshot);
    }

    Entity EntityDuplicateCommand::GetDuplicatedEntity() const
    {
        return m_Scene->FindEntityByUUID(m_DuplicateUUID);
    }
}
