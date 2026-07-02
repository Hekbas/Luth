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

    // ---- Helpers ----

    static json SerializeVec3(const Vec3& v)
    {
        return { v.x, v.y, v.z };
    }

    static Vec3 DeserializeVec3(const json& j, const Vec3& fallback = Vec3(0.0f))
    {
        if (!j.is_array() || j.size() < 3) return fallback;
        return { j[0].get<float>(), j[1].get<float>(), j[2].get<float>() };
    }

    // Quat serialization order is (w, x, y, z) to match glm::quat's constructor; readable by humans
    // and consistent with how the engine writes identity rotations.
    static json SerializeQuat(const Quat& q)
    {
        return { q.w, q.x, q.y, q.z };
    }

    static Quat DeserializeQuat(const json& j, const Quat& fallback = Quat(1.0f, 0.0f, 0.0f, 0.0f))
    {
        if (!j.is_array() || j.size() < 4) return fallback;
        return { j[0].get<float>(), j[1].get<float>(), j[2].get<float>(), j[3].get<float>() };
    }

    static const char* ColliderTypeToString(Collider::Type t)
    {
        switch (t)
        {
            case Collider::Type::Box:           return "Box";
            case Collider::Type::Sphere:        return "Sphere";
            case Collider::Type::Capsule:       return "Capsule";
            case Collider::Type::ConvexHullRef: return "ConvexHullRef";
            case Collider::Type::MeshRef:       return "MeshRef";
        }
        return "Box";
    }

    static const char* FogVolumeTypeToString(FogVolume::Type t)
    {
        switch (t)
        {
            case FogVolume::Type::Box:    return "Box";
            case FogVolume::Type::Sphere: return "Sphere";
        }
        return "Box";
    }

    static FogVolume::Type FogVolumeTypeFromString(const std::string& s)
    {
        if (s == "Sphere") return FogVolume::Type::Sphere;
        return FogVolume::Type::Box;
    }

    static Collider::Type ColliderTypeFromString(const std::string& s)
    {
        if (s == "Sphere")        return Collider::Type::Sphere;
        if (s == "Capsule")       return Collider::Type::Capsule;
        if (s == "ConvexHullRef") return Collider::Type::ConvexHullRef;
        if (s == "MeshRef")       return Collider::Type::MeshRef;
        return Collider::Type::Box;
    }

    static const char* MotionToString(RigidBody::Motion m)
    {
        switch (m)
        {
            case RigidBody::Motion::Static:    return "Static";
            case RigidBody::Motion::Kinematic: return "Kinematic";
            case RigidBody::Motion::Dynamic:   return "Dynamic";
        }
        return "Dynamic";
    }

    static RigidBody::Motion MotionFromString(const std::string& s)
    {
        if (s == "Static")    return RigidBody::Motion::Static;
        if (s == "Kinematic") return RigidBody::Motion::Kinematic;
        return RigidBody::Motion::Dynamic;
    }

    static const char* MotionQualityToString(RigidBody::Quality q)
    {
        switch (q)
        {
            case RigidBody::Quality::Discrete:   return "Discrete";
            case RigidBody::Quality::LinearCast: return "LinearCast";
        }
        return "Discrete";
    }

    static RigidBody::Quality MotionQualityFromString(const std::string& s)
    {
        if (s == "LinearCast") return RigidBody::Quality::LinearCast;
        return RigidBody::Quality::Discrete;
    }

    // ---- Serialize a Single Entity ----

    static json SerializeEntity(Entity entity)
    {
        json j;

        // Core data (always present)
        j["uuid"]   = entity.GetComponent<ID>().Value.ToString();
        j["tag"]    = entity.GetComponent<Tag>().Value;
        j["active"] = entity.IsActive();
        if (entity.HasComponent<Bone>()) j["bone"] = true;   // skeleton-joint marker

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
            cj["currentClipUUID"]           = ctrl.CurrentClipUUID.ToString();
            cj["applyRootMotion"]           = ctrl.ApplyRootMotion;
            cj["defaultTransitionDuration"] = ctrl.DefaultTransitionDuration;

            json layersJson = json::array();
            for (const auto& layer : ctrl.Layers) {
                json lj;
                lj["clipUUID"] = layer.ClipUUID.ToString();
                lj["speed"]    = layer.Speed;
                lj["weight"]   = layer.Weight;
                lj["loop"]     = layer.Loop;
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
            dj["shadowing"]           = static_cast<i32>(dl.Shadowing);
            dj["rtOriginEpsilon"]     = dl.RtOriginEpsilon;
            dj["rtNormalEpsilon"]     = dl.RtNormalEpsilon;
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

        if (entity.HasComponent<SpotLight>()) {
            auto& sl = entity.GetComponent<SpotLight>();
            json sj;
            sj["color"]     = SerializeVec3(sl.Color);
            sj["intensity"] = sl.Intensity;
            sj["range"]     = sl.Range;
            sj["innerCone"] = sl.InnerConeAngleDeg;
            sj["outerCone"] = sl.OuterConeAngleDeg;
            j["spotLight"]  = sj;
        }

        if (entity.HasComponent<FogVolume>()) {
            const auto& fv = entity.GetComponent<FogVolume>();
            json fj;
            fj["type"]           = FogVolumeTypeToString(fv.type);
            fj["localOffset"]    = SerializeVec3(fv.localOffset);
            fj["localRotation"]  = SerializeQuat(fv.localRotation);
            switch (fv.type)
            {
                case FogVolume::Type::Box:
                    fj["box"]    = json{ {"halfExtents", SerializeVec3(fv.halfExtents)} };
                    break;
                case FogVolume::Type::Sphere:
                    fj["sphere"] = json{ {"radius", fv.radius} };
                    break;
            }
            fj["color"]          = SerializeVec3(fv.color);
            fj["density"]        = fv.density;
            fj["falloffStart"]   = fv.falloffStart;
            fj["falloffEnd"]     = fv.falloffEnd;
            fj["affectsAmbient"] = fv.affectsAmbient;
            j["fogVolume"]       = fj;
        }

        if (entity.HasComponent<Wind>()) {
            const auto& w = entity.GetComponent<Wind>();
            json wj;
            wj["enabled"]              = w.enabled;
            wj["strengthMultiplier"]   = w.strengthMultiplier;
            wj["phaseOffset"]          = w.phaseOffset;
            wj["gustMultiplier"]       = w.gustMultiplier;
            wj["detailMultiplier"]     = w.detailMultiplier;
            wj["useDirectionOverride"] = w.useDirectionOverride;
            wj["directionOverride"]    = SerializeVec3(w.directionOverride);
            wj["overrideIsWorldSpace"] = w.overrideIsWorldSpace;
            j["wind"]                  = wj;
        }

        if (entity.HasComponent<Collider>()) {
            const auto& c = entity.GetComponent<Collider>();
            json cj;
            cj["type"]          = ColliderTypeToString(c.type);
            cj["localOffset"]   = SerializeVec3(c.localOffset);
            cj["localRotation"] = SerializeQuat(c.localRotation);

            // Only the active union member is serialized; loader picks the right field by `type`.
            switch (c.type)
            {
                case Collider::Type::Box:
                    cj["box"] = json{ {"halfExtents", SerializeVec3(c.boxHalfExtents)} };
                    break;
                case Collider::Type::Sphere:
                    cj["sphere"] = json{ {"radius", c.sphereRadius} };
                    break;
                case Collider::Type::Capsule:
                    cj["capsule"] = json{
                        {"radius",     c.capsule.radius},
                        {"halfHeight", c.capsule.halfHeight}
                    };
                    break;
                case Collider::Type::ConvexHullRef:
                case Collider::Type::MeshRef:
                {
                    UUID model(c.meshRef.modelHi, c.meshRef.modelLo);
                    cj["meshRef"] = json{
                        {"modelUUID", model.ToString()},
                        {"meshIndex", c.meshRef.meshIndex}
                    };
                    break;
                }
            }
            j["collider"] = cj;
        }

        if (entity.HasComponent<RigidBody>()) {
            const auto& rb = entity.GetComponent<RigidBody>();
            json rj;
            rj["motion"]          = MotionToString(rb.motion);
            rj["motionQuality"]   = MotionQualityToString(rb.motionQuality);
            rj["layer"]           = rb.layer;
            rj["isSensor"]        = rb.isSensor;
            rj["startActive"]     = rb.startActive;
            rj["mass"]            = rb.mass;
            rj["linearVelocity"]  = SerializeVec3(rb.linearVelocity);
            rj["angularVelocity"] = SerializeVec3(rb.angularVelocity);
            rj["gravityFactor"]   = rb.gravityFactor;
            rj["linearDamping"]   = rb.linearDamping;
            rj["angularDamping"]  = rb.angularDamping;
            rj["materialUUID"]    = rb.materialUUID.ToString();
            j["rigidBody"]        = rj;
        }

        // CharacterController: authoring fields only. desiredVelocity / jumpQueued (per-frame inputs)
        // and groundState / currentVelocity (read-back) are skipped; they're refreshed each frame by
        // PlayerControllerSystem and PhysicsSystem respectively.
        if (entity.HasComponent<CharacterController>()) {
            const auto& cc = entity.GetComponent<CharacterController>();
            json chj;
            chj["maxSlopeAngleDeg"]          = cc.maxSlopeAngleDeg;
            chj["mass"]                      = cc.mass;
            chj["maxStrength"]               = cc.maxStrength;
            chj["characterPadding"]          = cc.characterPadding;
            chj["predictiveContactDistance"] = cc.predictiveContactDistance;
            chj["penetrationRecoverySpeed"]  = cc.penetrationRecoverySpeed;
            chj["layer"]                     = cc.layer;
            chj["gravityFactor"]             = cc.gravityFactor;
            chj["moveSpeed"]                 = cc.moveSpeed;
            chj["jumpSpeed"]                 = cc.jumpSpeed;
            j["characterController"]         = chj;
        }

        return j;
    }

    // ---- Depth-First Traversal (parents before children) ----

    static void CollectEntitiesDFS(Entity entity, std::vector<Entity>& out)
    {
        out.push_back(entity);
        for (Entity child : entity.GetChildren()) {
            CollectEntitiesDFS(child, out);
        }
    }

    // ---- Save ----

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
            LH_LOG(Scene, error, "SceneSerializer::Save - failed to open '{}'", path.string());
            return false;
        }

        file << SaveToString(scene);
        file.close();

        LH_LOG(Scene, info, "Scene saved to '{}'", path.string());
        return true;
    }

    // ---- Load ----

    bool SceneSerializer::Load(Scene& scene, const fs::path& path)
    {
        std::ifstream file(path);
        if (!file.is_open()) {
            LH_LOG(Scene, error, "SceneSerializer::Load - failed to open '{}'", path.string());
            return false;
        }

        std::string contents((std::istreambuf_iterator<char>(file)),
                              std::istreambuf_iterator<char>());
        file.close();

        if (!LoadFromString(scene, contents, /*preserveAssets=*/false)) {
            LH_LOG(Scene, error, "SceneSerializer::Load - failed to load '{}'", path.string());
            return false;
        }

        LH_LOG(Scene, info, "Scene loaded from '{}'", path.string());
        return true;
    }

    bool SceneSerializer::LoadFromString(Scene& scene, std::string_view jsonStr, bool preserveAssets)
    {
        json root;
        try {
            root = json::parse(jsonStr);
        }
        catch (const json::parse_error& e) {
            LH_LOG(Scene, error, "SceneSerializer::LoadFromString - parse error: {}", e.what());
            return false;
        }

        if (!root.contains("entities") || !root["entities"].is_array()) {
            LH_LOG(Scene, error, "SceneSerializer::LoadFromString - invalid scene format (missing entities array)");
            return false;
        }

        // Clear existing scene. preserveAssets=true keeps Scene::m_HeldAssets alive; used by play-mode
        // Stop so AssetManager doesn't re-resolve every mesh/texture on restore.
        if (preserveAssets) scene.ClearPreservingAssets();
        else                scene.Clear();

        // ---- Pass 1: create all entities, populate components ----
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
            if (ej.value("bone", false)) entity.GetScene()->Registry().emplace<Bone>((entt::entity)entity);

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

                // Migrate from legacy `animationIndex`. LoadImmediate is blocking but only runs once per
                // scene load and only when scenes pre-date the clipUUID field.
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
                            LH_LOG(Scene, info, "SceneSerializer: migrated Animation index {} -> {} for {}",
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

                // Lazy migration helper: legacy `clipIndex` resolved against the companion Animation's model.
                // Cached after first call so a single controller with N layers triggers at most one LoadImmediate.
                UUID modelUUID;
                if (entity.HasComponent<Animation>())
                    modelUUID = entity.GetComponent<Animation>().ModelUUID;
                std::shared_ptr<Model> migrationModel;
                auto resolveLegacy = [&](i32 idx) -> UUID {
                    if (idx < 0 || !modelUUID.IsValid()) return UUID();
                    if (!migrationModel) {
                        auto loaded = AssetManager::LoadImmediate(modelUUID);
                        migrationModel = std::dynamic_pointer_cast<Model>(loaded);
                    }
                    if (!migrationModel) return UUID();
                    const auto& uuids = migrationModel->GetAnimationClipUUIDs();
                    return ((u32)idx < uuids.size()) ? uuids[idx] : UUID();
                };

                if (cj.contains("currentClipUUID"))
                    ctrl.CurrentClipUUID = UUID::FromString(cj.value("currentClipUUID", ""));
                else if (cj.contains("currentClipIndex"))
                    ctrl.CurrentClipUUID = resolveLegacy(cj.value("currentClipIndex", -1));

                ctrl.ApplyRootMotion           = cj.value("applyRootMotion", false);
                ctrl.DefaultTransitionDuration = cj.value("defaultTransitionDuration", 0.2f);

                if (cj.contains("layers")) {
                    for (const auto& lj : cj["layers"]) {
                        BlendLayer layer;
                        if (lj.contains("clipUUID"))
                            layer.ClipUUID = UUID::FromString(lj.value("clipUUID", ""));
                        else if (lj.contains("clipIndex"))
                            layer.ClipUUID = resolveLegacy(lj.value("clipIndex", -1));
                        layer.Speed  = lj.value("speed", 1.0f);
                        layer.Weight = lj.value("weight", 1.0f);
                        layer.Loop   = lj.value("loop", true);
                        if (lj.contains("boneMask")) {
                            // Sparse mask: array of enabled bone indices. The full vector is reconstructed
                            // once the skeleton is available; for now size the vector from the max index.
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

                // RT-shadow toggle + epsilons. Missing key -> engine default (RtShadows) so legacy
                // scenes pick up RT automatically; saved value round-trips otherwise.
                dl.Shadowing       = static_cast<ShadowingMode>(
                                         dj.value("shadowing", static_cast<i32>(ShadowingMode::RtShadows)));
                dl.RtOriginEpsilon = dj.value("rtOriginEpsilon", 0.001f);
                dl.RtNormalEpsilon = dj.value("rtNormalEpsilon", 0.05f);
            }

            // PointLight
            if (ej.contains("pointLight")) {
                const auto& pj = ej["pointLight"];
                auto& pl = entity.AddComponent<PointLight>();
                pl.Color     = DeserializeVec3(pj.value("color", json::array()), { 1, 1, 1 });
                pl.Intensity = pj.value("intensity", 1.0f);
                pl.Range     = pj.value("range", 350.0f);
            }

            // SpotLight
            if (ej.contains("spotLight")) {
                const auto& sj = ej["spotLight"];
                auto& sl = entity.AddComponent<SpotLight>();
                sl.Color             = DeserializeVec3(sj.value("color", json::array()), { 1, 1, 1 });
                sl.Intensity         = sj.value("intensity", 1.0f);
                sl.Range             = sj.value("range", 350.0f);
                sl.InnerConeAngleDeg = sj.value("innerCone", 25.0f);
                sl.OuterConeAngleDeg = sj.value("outerCone", 45.0f);
            }

            // FogVolume: tagged union; load type first, then the active union member.
            if (ej.contains("fogVolume")) {
                const auto& fj = ej["fogVolume"];
                FogVolume fv;
                fv.type          = FogVolumeTypeFromString(fj.value("type", "Box"));
                fv.localOffset   = DeserializeVec3(fj.value("localOffset",   json::array()), Vec3(0.0f));
                fv.localRotation = DeserializeQuat(fj.value("localRotation", json::array()));
                switch (fv.type)
                {
                    case FogVolume::Type::Box:
                        if (fj.contains("box"))
                            fv.halfExtents = DeserializeVec3(fj["box"].value("halfExtents", json::array()),
                                                             Vec3(2.0f));
                        break;
                    case FogVolume::Type::Sphere:
                        if (fj.contains("sphere"))
                            fv.radius = fj["sphere"].value("radius", 2.0f);
                        break;
                }
                fv.color          = DeserializeVec3(fj.value("color", json::array()), Vec3(1.0f));
                fv.density        = fj.value("density",        0.1f);
                fv.falloffStart   = fj.value("falloffStart",   0.0f);
                fv.falloffEnd     = fj.value("falloffEnd",     1.0f);
                fv.affectsAmbient = fj.value("affectsAmbient", true);
                entity.AddComponent<FogVolume>(fv);
            }

            // Wind: per-entity response to the global field (additive; absent -> full global response).
            if (ej.contains("wind")) {
                const auto& wj = ej["wind"];
                Wind w;
                w.enabled              = wj.value("enabled", true);
                w.strengthMultiplier   = wj.value("strengthMultiplier", 1.0f);
                w.phaseOffset          = wj.value("phaseOffset", 0.0f);
                w.gustMultiplier       = wj.value("gustMultiplier", 1.0f);
                w.detailMultiplier     = wj.value("detailMultiplier", 1.0f);
                w.useDirectionOverride = wj.value("useDirectionOverride", false);
                w.directionOverride    = DeserializeVec3(wj.value("directionOverride", json::array()),
                                                         Vec3(1.0f, 0.0f, 0.0f));
                w.overrideIsWorldSpace = wj.value("overrideIsWorldSpace", true);
                entity.AddComponent<Wind>(w);
            }

            // Collider / RigidBody: both populate a local first, then AddComponent copy-emplaces the
            // populated value. EnTT's on_construct fires synchronously inside AddComponent, so the
            // "AddComponent + then assign fields" pattern used by other components above leaves the
            // signal handler reading defaults. PhysicsSystem's TryCreateBody runs in that signal,
            // and reading defaults instead of the loaded values means a Static body in JSON ends up
            // built as Dynamic (the default Motion).
            if (ej.contains("collider")) {
                const auto& cj = ej["collider"];
                Collider c;
                c.type           = ColliderTypeFromString(cj.value("type", "Box"));
                c.localOffset    = DeserializeVec3(cj.value("localOffset",   json::array()), Vec3(0.0f));
                c.localRotation  = DeserializeQuat(cj.value("localRotation", json::array()));

                switch (c.type)
                {
                    case Collider::Type::Box:
                        if (cj.contains("box"))
                            c.boxHalfExtents = DeserializeVec3(cj["box"].value("halfExtents", json::array()),
                                                               Vec3(0.5f));
                        break;
                    case Collider::Type::Sphere:
                        if (cj.contains("sphere"))
                            c.sphereRadius = cj["sphere"].value("radius", 0.5f);
                        break;
                    case Collider::Type::Capsule:
                        if (cj.contains("capsule"))
                        {
                            c.capsule.radius     = cj["capsule"].value("radius",     0.5f);
                            c.capsule.halfHeight = cj["capsule"].value("halfHeight", 0.5f);
                        }
                        break;
                    case Collider::Type::ConvexHullRef:
                    case Collider::Type::MeshRef:
                        if (cj.contains("meshRef"))
                        {
                            UUID model = UUID::FromString(cj["meshRef"].value("modelUUID", ""));
                            c.meshRef.modelHi   = model.GetHalf0();
                            c.meshRef.modelLo   = model.GetHalf1();
                            c.meshRef.meshIndex = cj["meshRef"].value("meshIndex", 0u);
                        }
                        break;
                }
                entity.AddComponent<Collider>(c);
            }

            if (ej.contains("rigidBody")) {
                const auto& rj = ej["rigidBody"];
                RigidBody rb;
                rb.motion          = MotionFromString(rj.value("motion", "Dynamic"));
                rb.motionQuality   = MotionQualityFromString(rj.value("motionQuality", "Discrete"));
                rb.layer           = rj.value("layer", static_cast<u8>(1));
                rb.isSensor        = rj.value("isSensor", false);
                rb.startActive     = rj.value("startActive", true);
                rb.mass            = rj.value("mass", 0.0f);
                rb.linearVelocity  = DeserializeVec3(rj.value("linearVelocity",  json::array()), Vec3(0.0f));
                rb.angularVelocity = DeserializeVec3(rj.value("angularVelocity", json::array()), Vec3(0.0f));
                rb.gravityFactor   = rj.value("gravityFactor",  1.0f);
                rb.linearDamping   = rj.value("linearDamping",  0.05f);
                rb.angularDamping  = rj.value("angularDamping", 0.05f);
                rb.materialUUID    = UUID::FromString(rj.value("materialUUID", ""));
                entity.AddComponent<RigidBody>(rb);
            }

            // Populate local CC, then AddComponent; EnTT on_construct must see deserialized values,
            // not defaults, or PhysicsSystem's first build would queue with stale fields.
            if (ej.contains("characterController")) {
                const auto& chj = ej["characterController"];
                CharacterController cc;
                cc.maxSlopeAngleDeg          = chj.value("maxSlopeAngleDeg",          45.0f);
                cc.mass                      = chj.value("mass",                      70.0f);
                cc.maxStrength               = chj.value("maxStrength",               100.0f);
                cc.characterPadding          = chj.value("characterPadding",          0.02f);
                cc.predictiveContactDistance = chj.value("predictiveContactDistance", 0.1f);
                cc.penetrationRecoverySpeed  = chj.value("penetrationRecoverySpeed",  1.0f);
                cc.layer                     = chj.value("layer", static_cast<u8>(1));
                cc.gravityFactor             = chj.value("gravityFactor",             1.0f);
                cc.moveSpeed                 = chj.value("moveSpeed",                 5.0f);
                cc.jumpSpeed                 = chj.value("jumpSpeed",                 6.0f);
                entity.AddComponent<CharacterController>(cc);
            }

            // Store for hierarchy reconstruction
            uuidToEntity[uuidStr] = entity;

            std::string parentUUID = ej.value("parent", "");
            if (!parentUUID.empty()) {
                parentLinks.push_back({ entity, parentUUID });
            }
        }

        // ---- Pass 2: reconstruct hierarchy ----
        for (auto& [child, parentUUID] : parentLinks) {
            auto it = uuidToEntity.find(parentUUID);
            if (it != uuidToEntity.end()) {
                child.SetParent(it->second);
            }
            else {
                LH_LOG(Scene, warn, "SceneSerializer::LoadFromString - parent UUID '{}' not found for entity '{}'",
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
                LH_LOG(Scene, warn, "SceneSerializer::LoadFromString - BoneAttachment target UUID '{}' not found for '{}'",
                    targetUUID, child.GetName());
            }
        }

        LH_LOG(Scene, info, "Scene loaded ({} entities)", uuidToEntity.size());
        return true;
    }
}
