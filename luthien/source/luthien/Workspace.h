#pragma once

#include <string>
#include <filesystem>
#include <unordered_map>

namespace Luth
{
    // Lightweight enumeration record returned by Editor::GetWorkspaces().
    // builtin = shipped under luth/assets/workspaces/, immutable from the editor UI.
    struct WorkspaceInfo
    {
        std::string name;
        bool        builtin = false;
    };

    // Workspace = ImGui dock layout (.ini) + sidecar JSON holding the per-panel
    // visibility set. Pair format keeps the .ini ImGui-native; sidecar evolves
    // freely. Active name lives in EditorSettings::activeLayout.
    namespace Workspace
    {
        // Sidecar JSON shape: { "panel_open": { "<window-id>": <bool>, ... } }.
        // LoadJson leaves outPanelOpen untouched when the file is missing — callers
        // treat that as "no sidecar; keep existing in-memory state" so legacy .ini-only
        // workspaces don't clobber visibility on first load.
        bool LoadJson(const std::filesystem::path& jsonPath,
                      std::unordered_map<std::string, bool>& outPanelOpen);
        bool SaveJson(const std::filesystem::path& jsonPath,
                      const std::unordered_map<std::string, bool>& panelOpen);

        // True when path resolves under FileSystem::EngineAssetsPath("workspaces").
        // Built-ins are read-only — Rename / Delete must reject them.
        bool IsBuiltinPath(const std::filesystem::path& iniOrJsonPath);
    }
}
