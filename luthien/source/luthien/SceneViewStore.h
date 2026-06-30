#pragma once

#include "luthien/EditorCamera.h"

#include <filesystem>
#include <optional>
#include <string>
#include <unordered_map>

namespace Luth
{
    // Per-project persisted editor scene-view camera poses, keyed by scene UUID string. Lives at
    // <project>/.luth/scene_views.json — gitignored, per-user, never in the committed scene asset
    // (mirrors Unity, which keeps Scene View camera state in Library/). Editor write-throughs on
    // scene save/switch/shutdown and loads the active project's file on project change.
    class SceneViewStore
    {
    public:
        std::optional<EditorCameraPose> Get(const std::string& sceneUUID) const;
        void Set(const std::string& sceneUUID, const EditorCameraPose& pose);

        // Replaces all entries from disk; a missing file leaves the store empty (no warning).
        void Load(const std::filesystem::path& path);
        // Creates the parent directory if absent, then writes pretty JSON.
        void Save(const std::filesystem::path& path) const;
        void Clear() { m_Poses.clear(); }

    private:
        std::unordered_map<std::string, EditorCameraPose> m_Poses;
    };
}
