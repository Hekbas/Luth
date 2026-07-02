#include "lepch.h"
#include "luthien/SceneViewStore.h"

#include <nlohmann/json.hpp>
#include <fstream>

namespace Luth
{
    using json = nlohmann::json;

    namespace
    {
        json ToJson(const Vec3& v) { return json::array({ v.x, v.y, v.z }); }

        // Vec3 from JSON array. Returns the fallback on a malformed entry so a corrupt file leaves
        // pose defaults intact rather than throwing.
        Vec3 ToVec3(const json& j, const Vec3& fallback)
        {
            if (j.is_array() && j.size() == 3)
                return Vec3(j[0].get<float>(), j[1].get<float>(), j[2].get<float>());
            return fallback;
        }
    }

    std::optional<EditorCameraPose> SceneViewStore::Get(const std::string& sceneUUID) const
    {
        auto it = m_Poses.find(sceneUUID);
        if (it == m_Poses.end()) return std::nullopt;
        return it->second;
    }

    void SceneViewStore::Set(const std::string& sceneUUID, const EditorCameraPose& pose)
    {
        m_Poses[sceneUUID] = pose;
    }

    void SceneViewStore::Load(const std::filesystem::path& path)
    {
        m_Poses.clear();

        if (!std::filesystem::exists(path))
            return;

        try
        {
            std::ifstream file(path);
            json j = json::parse(file);

            if (j.contains("scene_views") && j["scene_views"].is_object())
            {
                for (auto it = j["scene_views"].begin(); it != j["scene_views"].end(); ++it)
                {
                    const json& e = it.value();
                    if (!e.is_object()) continue;

                    EditorCameraPose pose;
                    if (e.contains("focal")) pose.focalPoint = ToVec3(e["focal"], pose.focalPoint);
                    pose.distance = e.value("distance", pose.distance);
                    pose.pitch    = e.value("pitch", pose.pitch);
                    pose.yaw      = e.value("yaw", pose.yaw);
                    m_Poses[it.key()] = pose;
                }
            }

            LH_LOG(Editor, info, "Loaded {} scene-view camera pose(s) from '{}'",
                   m_Poses.size(), path.string());
        }
        catch (const std::exception& e)
        {
            LH_LOG(Editor, error, "Failed to load scene views: {}", e.what());
        }
    }

    void SceneViewStore::Save(const std::filesystem::path& path) const
    {
        try
        {
            // .luth/ may not exist yet on a fresh project; the bare ofstream below would
            // silently no-op without this (same gap EditorSettings::Save has).
            if (path.has_parent_path())
                std::filesystem::create_directories(path.parent_path());

            json views = json::object();
            for (const auto& [uuid, pose] : m_Poses)
            {
                json e;
                e["focal"]    = ToJson(pose.focalPoint);
                e["distance"] = pose.distance;
                e["pitch"]    = pose.pitch;
                e["yaw"]      = pose.yaw;
                views[uuid] = e;
            }

            json j;
            j["version"]     = 1;
            j["scene_views"] = views;

            std::ofstream file(path);
            file << j.dump(4);
        }
        catch (const std::exception& e)
        {
            LH_LOG(Editor, error, "Failed to save scene views: {}", e.what());
        }
    }
}
