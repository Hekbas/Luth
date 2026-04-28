#include "luthpch.h"
#include "luth/scene/SceneSerializer.h"
#include "luth/scene/Scene.h"
#include "luth/scene/Entity.h"
#include "luth/scene/Components.h"
#include "luth/resources/AssetManager.h"
#include "luth/renderer/resources/Model.h"

#include <nlohmann/json.hpp>
#include <fstream>

namespace Luth
{
    using json = nlohmann::json;
    using namespace Component;

    // ── Helpers ──────────────────────────────────────────────────

    static json SerializeVec3(const Vec3& v)
    {
        return { v.x, v.y, v.z };
    }

    static Vec3 DeserializeVec3(const json& j, const Vec3& fallback = Vec3(0.0f))
    {
        if (!j.is_array() || j.size() < 3) return fallback;
        return { j[0].get<float>(), j[1].get<float>(), j[2].get<float>() };
    }

    // ── Serialize a single entity ───────────────────────────────

    static json SerializeEntity(Entity entity)
    {
        json j;

        // Core data (always present)
        j["uuid"]   = entity.GetComponent<ID>().Value.ToString();
        j["tag"]    = entity.GetComponent<Tag>().Value;
        j["active"] = entity.IsActive();

        // Parent UUID (empty string if root)
        if (entity.HasParent()) {
            Entity parent = entity.GetParent();
            j["parent"] = parent.GetComponent<ID>().Value.ToString();
        }
        else {
            j["parent"] = "";
        }

        // Transform (always present after CreateEntity)
        {
            auto& t = entity.GetComponent<Transform>();
            json tj;
            tj["position"] = SerializeVec3(t.Position);
            tj["rotation"] = SerializeVec3(t.Rotation);
            tj["scale"]    = SerializeVec3(t.Scale);
            j["transform"] = tj;
        }

        // Optional components
        if (entity.HasComponent<Camera>()) {
            auto& c = entity.GetComponent<Camera>();
            json cj;
            cj["projection"]      = static_cast<int>(c.Projection);
            cj["verticalFOV"]     = c.VerticalFOV;
            cj["nearClip"]        = c.NearClip;
            cj["farClip"]         = c.FarClip;
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
            aj["modelUUID"] = a.ModelUUID.ToString();
            aj["clipUUID"]  = a.ClipUUID.ToString();
            aj["speed"]     = a.Speed;
            aj["loopMode"]  = static_cast<int>(a.LoopMode);
            aj["playing"]   = a.Playing;
            j["animation"]  = aj;
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
            if (ba.TargetEntity) {
                baj["targetUUID"] = ba.TargetEntity.GetComponent<ID>().Value.ToString();
            }
            baj["boneName"]      = ba.BoneName;
            baj["localOffset"]   = SerializeVec3(ba.LocalOffset);
            baj["localRotation"] = SerializeVec3(ba.LocalRotation);
            j["boneAttachment"]  = baj;
        }

        if (entity.HasComponent<DirectionalLight>()) {
            auto& dl = entity.GetComponent<DirectionalLight>();
            json dj;
            dj["color"]           = SerializeVec3(dl.Color);
            dj["intensity"]       = dl.Intensity;
            dj["castShadows"]     = dl.CastShadows;
            dj["shadowOrthoSize"] = dl.ShadowOrthoSize;
            dj["shadowDistance"]  = dl.ShadowDistance;
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
            pj["color"]     = SerializeVec3(pl.Color);
            pj["intensity"] = pl.Intensity;
            pj["range"]     = pl.Range;
            j["pointLight"] = pj;
        }

        return j;
    }

    // ── Depth-first traversal (parents before children) ─────────

    static void CollectEntitiesDFS(Entity entity, std::vector<Entity>& out)
    {
        out.push_back(entity);
        for (Entity child : entity.GetChildren()) {
            CollectEntitiesDFS(child, out);
        }
    }

    // ── Save ────────────────────────────────────────────────────

    std::string SceneSerializer::SaveToString(const Scene& scene)
    {
        // Collect entities in depth-first order (parents before children)
        std::vector<Entity> ordered;
        for (Entity root : scene.GetRootEntities()) {
            CollectEntitiesDFS(root, ordered);
        }

        json root;
        root["version"] = 1;

        json entities = json::array();
        for (Entity entity : ordered) {
            entities.push_back(SerializeEntity(entity));
        }
        root["entities"] = entities;

        return root.dump(4);
    }

    bool SceneSerializer::Save(const Scene& scene, const fs::path& path)
    {
        std::ofstream file(path);
        if (!file.is_open()) {
            LH_CORE_ERROR("SceneSerializer::Save — failed to open '{}'", path.string());
            return false;
        }

        file << SaveToString(scene);
        file.close();

        LH_CORE_INFO("Scene saved to '{}'", path.string());
        return true;
    }

    // ── Load ────────────────────────────────────────────────────

    bool SceneSerializer::Load(Scene& scene, const fs::path& path)
    {
        std::ifstream file(path);
        if (!file.is_open()) {
            LH_CORE_ERROR("SceneSerializer::Load — failed to open '{}'", path.string());
            return false;
        }

        std::string contents((std::istreambuf_iterator<char>(file)),
                              std::istreambuf_iterator<char>());
        file.close();

        if (!LoadFromString(scene, contents, /*preserveAssets=*/false)) {
            LH_CORE_ERROR("SceneSerializer::Load — failed to load '{}'", path.string());
            return false;
        }

        LH_CORE_INFO("Scene loaded from '{}'", path.string());
        return true;
    }

    bool SceneSerializer::LoadFromString(Scene& scene, std::string_view jsonStr, bool preserveAssets)
    {
        json root;
        try {
            root = json::parse(jsonStr);
        }
        catch (const json::parse_error& e) {
            LH_CORE_ERROR("SceneSerializer::LoadFromString — parse error: {}", e.what());
            return false;
        }

        if (!root.contains("entities") || !root["entities"].is_array()) {
            LH_CORE_ERROR("SceneSerializer::LoadFromString — invalid scene format (missing entities array)");
            return false;
        }

        // ── Clear existing scene ────────────────────────────────
        // preserveAssets=true keeps Scene::m_HeldAssets alive — used by play-mode
        // Stop so AssetManager doesn't re-resolve every mesh/texture on restore.
        if (preserveAssets) scene.ClearPreservingAssets();
        else                scene.Clear();

        // ── Pass 1: Create all entities, populate components ────
        std::unordered_map<std::string, Entity> uuidToEntity;
        std::vector<std::pair<Entity, std::string>> parentLinks;     // {child, parentUUID}
        std::vector<std::pair<Entity, std::string>> attachmentLinks; // {child, targetUUID}

        for (const auto& ej : root["entities"]) {
            std::string tag = ej.value("tag", "Entity");
            Entity entity = scene.CreateEntity(tag);

            // Overwrite auto-generated UUID
            std::string uuidStr = ej.value("uuid", "");
            if (!uuidStr.empty()) {
                entity.GetComponent<ID>().Value = UUID::FromString(uuidStr);
            }

            // Active state
            entity.SetActive(ej.value("active", true));

            // Transform (overwrite defaults)
            if (ej.contains("transform")) {
                auto& t = entity.GetComponent<Transform>();
                const auto& tj = ej["transform"];
                t.Position = DeserializeVec3(tj.value("position", json::array()), { 0, 0, 0 });
                t.Rotation = DeserializeVec3(tj.value("rotation", json::array()), { 0, 0, 0 });
                t.Scale    = DeserializeVec3(tj.value("scale",    json::array()), { 1, 1, 1 });
                t.IsDirty = true;
            }

            // Camera
            if (ej.contains("camera")) {
                const auto& cj = ej["camera"];
                auto& c = entity.AddComponent<Camera>();
                c.Projection      = static_cast<Camera::ProjectionType>(cj.value("projection", 0));
                c.VerticalFOV     = cj.value("verticalFOV", 45.0f);
                c.NearClip        = cj.value("nearClip", 0.01f);
                c.FarClip         = cj.value("farClip", 1000.0f);
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
                a.ModelUUID = UUID::FromString(aj.value("modelUUID", ""));

                // Migrate from legacy `animationIndex`. LoadImmediate is blocking but
                // only runs once per scene load and only when scenes pre-date this epic.
                if (aj.contains("clipUUID")) {
                    a.ClipUUID = UUID::FromString(aj.value("clipUUID", ""));
                }
                else if (aj.contains("animationIndex") && a.ModelUUID.IsValid()) {
                    auto loaded = AssetManager::LoadImmediate(a.ModelUUID);
                    auto modelPtr = std::dynamic_pointer_cast<Model>(loaded);
                    if (modelPtr) {
                        i32 idx = aj.value("animationIndex", 0);
                        const auto& uuids = modelPtr->GetAnimationClipUUIDs();
                        if (idx >= 0 && (u32)idx < uuids.size()) {
                            a.ClipUUID = uuids[idx];
                            LH_CORE_INFO("SceneSerializer: migrated Animation index {} -> {} for {}",
                                idx, a.ClipUUID.ToString(), a.ModelUUID.ToString());
                        }
                    }
                }

                a.Speed = aj.value("speed", 1.0f);
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
                            // Sparse mask: array of bone indices that are enabled
                            // We'll reconstruct the full vector once skeleton is available.
                            // For now, find the max index to size the vector.
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
                        ctrl.Layers.push_back(std::move(layer));
                    }
                }
            }

            // BoneAttachment
            if (ej.contains("boneAttachment")) {
                const auto& baj = ej["boneAttachment"];
                auto& ba = entity.AddComponent<BoneAttachment>();
                ba.BoneName      = baj.value("boneName", "");
                ba.LocalOffset   = DeserializeVec3(baj.value("localOffset", json::array()), Vec3(0));
                ba.LocalRotation = DeserializeVec3(baj.value("localRotation", json::array()), Vec3(0));
                std::string targetUUID = baj.value("targetUUID", "");
                if (!targetUUID.empty())
                    attachmentLinks.push_back({ entity, targetUUID });
            }

            // DirectionalLight
            if (ej.contains("directionalLight")) {
                const auto& dj = ej["directionalLight"];
                auto& dl = entity.AddComponent<DirectionalLight>();
                dl.Color           = DeserializeVec3(dj.value("color", json::array()), { 1, 1, 1 });
                dl.Intensity       = dj.value("intensity", 1.0f);
                dl.CastShadows     = dj.value("castShadows", true);
                dl.ShadowOrthoSize = dj.value("shadowOrthoSize", 200.0f);
                dl.ShadowDistance  = dj.value("shadowDistance", 200.0f);
                dl.SplitLambda        = dj.value("splitLambda", 0.5f);
                dl.StabilizeCascades  = dj.value("stabilizeCascades", true);

                // ShadowBias: array (new) or scalar (legacy, splay into all 4 slots)
                if (dj.contains("shadowBias") && dj["shadowBias"].is_array()) {
                    const auto& arr = dj["shadowBias"];
                    for (u32 i = 0; i < 4; ++i)
                        dl.ShadowBias[i] = (i < arr.size()) ? arr[i].get<float>() : dl.ShadowBias[i];
                }
                else {
                    float legacy = dj.value("shadowBias", 0.005f);
                    for (u32 i = 0; i < 4; ++i) dl.ShadowBias[i] = legacy;
                }

                // ShadowNormalBias: array (new) or keep defaults
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
                pl.Color     = DeserializeVec3(pj.value("color", json::array()), { 1, 1, 1 });
                pl.Intensity = pj.value("intensity", 1.0f);
                pl.Range     = pj.value("range", 350.0f);
            }

            // Store for hierarchy reconstruction
            uuidToEntity[uuidStr] = entity;

            std::string parentUUID = ej.value("parent", "");
            if (!parentUUID.empty()) {
                parentLinks.push_back({ entity, parentUUID });
            }
        }

        // ── Pass 2: Reconstruct hierarchy ───────────────────────
        for (auto& [child, parentUUID] : parentLinks) {
            auto it = uuidToEntity.find(parentUUID);
            if (it != uuidToEntity.end()) {
                child.SetParent(it->second);
            }
            else {
                LH_CORE_WARN("SceneSerializer::LoadFromString — parent UUID '{}' not found for entity '{}'",
                    parentUUID, child.GetName());
            }
        }

        // Resolve bone attachment targets
        for (auto& [child, targetUUID] : attachmentLinks) {
            auto it = uuidToEntity.find(targetUUID);
            if (it != uuidToEntity.end()) {
                child.GetComponent<BoneAttachment>().TargetEntity = it->second;
            }
            else {
                LH_CORE_WARN("SceneSerializer::LoadFromString — BoneAttachment target UUID '{}' not found for '{}'",
                    targetUUID, child.GetName());
            }
        }

        LH_CORE_INFO("Scene loaded ({} entities)", uuidToEntity.size());
        return true;
    }
}
