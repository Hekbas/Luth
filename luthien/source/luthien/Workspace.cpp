#include "lepch.h"
#include "luthien/Workspace.h"

#include "luth/resources/FileSystem.h"

#include <nlohmann/json.hpp>

namespace Luth::Workspace
{
    namespace fs = std::filesystem;
    using json = nlohmann::json;

    bool LoadJson(const fs::path& jsonPath,
                  std::unordered_map<std::string, bool>& outPanelOpen)
    {
        if (!fs::exists(jsonPath))
            return false;

        try {
            std::ifstream f(jsonPath);
            json j = json::parse(f);

            if (j.contains("panel_open") && j["panel_open"].is_object()) {
                outPanelOpen.clear();
                for (auto it = j["panel_open"].begin(); it != j["panel_open"].end(); ++it)
                    if (it.value().is_boolean())
                        outPanelOpen[it.key()] = it.value().get<bool>();
            }
            return true;
        }
        catch (const std::exception& e) {
            LH_CORE_ERROR("Workspace::LoadJson failed for '{}': {}", jsonPath.string(), e.what());
            return false;
        }
    }

    bool SaveJson(const fs::path& jsonPath,
                  const std::unordered_map<std::string, bool>& panelOpen)
    {
        try {
            if (jsonPath.has_parent_path())
                fs::create_directories(jsonPath.parent_path());

            json po = json::object();
            for (const auto& [name, open] : panelOpen)
                po[name] = open;

            json j;
            j["panel_open"] = po;

            std::ofstream f(jsonPath);
            f << j.dump(4);
            return true;
        }
        catch (const std::exception& e) {
            LH_CORE_ERROR("Workspace::SaveJson failed for '{}': {}", jsonPath.string(), e.what());
            return false;
        }
    }

    bool IsBuiltinPath(const fs::path& path)
    {
        std::error_code ec;
        const fs::path builtinDir = FileSystem::EngineAssetsPath("workspaces").lexically_normal();
        const fs::path normalized = path.lexically_normal();
        const fs::path rel = fs::relative(normalized, builtinDir, ec);

        if (ec || rel.empty()) return false;
        // fs::relative emits a leading ".." segment when target lies outside base.
        const auto first = rel.begin();
        return first != rel.end() && *first != "..";
    }
}
