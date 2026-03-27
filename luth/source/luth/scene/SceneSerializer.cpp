#include "luthpch.h"
#include "luth/scene/SceneSerializer.h"
#include "luth/scene/Scene.h"
#include "luth/scene/Entity.h"
#include "luth/scene/Components.h"

#include <nlohmann/json.hpp>
#include <fstream>

namespace Luth
{
    using json = nlohmann::json;
    using namespace Component;

    // ── Helpers ──────────────────────────────────────────────────

    static json SerializeVec3(const glm::vec3& v)
    {
        return { v.x, v.y, v.z };
    }

    static glm::vec3 DeserializeVec3(const json& j, const glm::vec3& fallback = glm::vec3(0.0f))
    {
        if (!j.is_array() || j.size() < 3) return fallback;
        return { j[0].get<float>(), j[1].get<float>(), j[2].get<float>() };
    }

    // ── Serialize a single entity ───────────────────────────────

    static json SerializeEntity(Entity entity)
    {
        json j;

        // Core data (always present)
        j["uuid"]   = entity.GetComponent<ID>().m_ID.ToString();
        j["tag"]    = entity.GetComponent<Tag>().m_Tag;
        j["active"] = entity.IsActive();

        // Parent UUID (empty string if root)
        if (entity.HasParent()) {
            Entity parent = entity.GetParent();
            j["parent"] = parent.GetComponent<ID>().m_ID.ToString();
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
            aj["modelUUID"]      = a.ModelUUID.ToString();
            aj["animationIndex"] = a.AnimationIndex;
            aj["speed"]          = a.Speed;
            aj["loop"]           = a.Loop;
            aj["playing"]        = false;  // Always save as paused
            j["animation"]       = aj;
        }

        if (entity.HasComponent<DirectionalLight>()) {
            auto& dl = entity.GetComponent<DirectionalLight>();
            json dj;
            dj["color"]           = SerializeVec3(dl.Color);
            dj["intensity"]       = dl.Intensity;
            dj["castShadows"]     = dl.CastShadows;
            dj["shadowBias"]      = dl.ShadowBias;
            dj["shadowOrthoSize"] = dl.ShadowOrthoSize;
            dj["shadowDistance"]  = dl.ShadowDistance;
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

    bool SceneSerializer::Save(const Scene& scene, const fs::path& path)
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

        std::ofstream file(path);
        if (!file.is_open()) {
            LH_CORE_ERROR("SceneSerializer::Save — failed to open '{}'", path.string());
            return false;
        }

        file << root.dump(4);
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

        json root;
        try {
            root = json::parse(file);
        }
        catch (const json::parse_error& e) {
            LH_CORE_ERROR("SceneSerializer::Load — parse error: {}", e.what());
            return false;
        }
        file.close();

        if (!root.contains("entities") || !root["entities"].is_array()) {
            LH_CORE_ERROR("SceneSerializer::Load — invalid scene format (missing entities array)");
            return false;
        }

        // ── Clear existing scene ────────────────────────────────
        scene.Clear();

        // ── Pass 1: Create all entities, populate components ────
        std::unordered_map<std::string, Entity> uuidToEntity;
        std::vector<std::pair<Entity, std::string>> parentLinks; // {child, parentUUID}

        for (const auto& ej : root["entities"]) {
            std::string tag = ej.value("tag", "Entity");
            Entity entity = scene.CreateEntity(tag);

            // Overwrite auto-generated UUID
            std::string uuidStr = ej.value("uuid", "");
            if (!uuidStr.empty()) {
                entity.GetComponent<ID>().m_ID = UUID::FromString(uuidStr);
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
                a.ModelUUID      = UUID::FromString(aj.value("modelUUID", ""));
                a.AnimationIndex = aj.value("animationIndex", 0);
                a.Speed          = aj.value("speed", 1.0f);
                a.Loop           = aj.value("loop", true);
                a.Playing        = aj.value("playing", false);
            }

            // DirectionalLight
            if (ej.contains("directionalLight")) {
                const auto& dj = ej["directionalLight"];
                auto& dl = entity.AddComponent<DirectionalLight>();
                dl.Color           = DeserializeVec3(dj.value("color", json::array()), { 1, 1, 1 });
                dl.Intensity       = dj.value("intensity", 1.0f);
                dl.CastShadows     = dj.value("castShadows", true);
                dl.ShadowBias      = dj.value("shadowBias", 0.005f);
                dl.ShadowOrthoSize = dj.value("shadowOrthoSize", 200.0f);
                dl.ShadowDistance   = dj.value("shadowDistance", 200.0f);
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
                LH_CORE_WARN("SceneSerializer::Load — parent UUID '{}' not found for entity '{}'",
                    parentUUID, child.GetName());
            }
        }

        LH_CORE_INFO("Scene loaded from '{}' ({} entities)", path.string(), uuidToEntity.size());
        return true;
    }
}
