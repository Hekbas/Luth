#pragma once

#include "luth/core/types/LuthTypes.h"

#include <filesystem>
#include <string>

namespace Luth
{
    class RenderingSystem;

    // Disk persistence for every user-facing render setting on RenderingSystem, plus the
    // ViewRenderSettings that used to be editor-only. Exists so a headless capture renders the same
    // configuration the editor does: before this, none of these structs were serialized anywhere, so
    // LuthShot always ran the compiled defaults and no capture could be compared against a viewport.
    //
    // Load and Save share one field visitor per struct (see the .cpp), so a field cannot be readable
    // but not writable. SlangParitySettings is deliberately excluded: it is computed diagnostic state,
    // not user intent. see arch/rendering-pipeline.md
    namespace RenderSettingsIO
    {
        // <project>/.luth/render.json
        std::filesystem::path DefaultPath(const std::filesystem::path& projectRoot);

        // Missing file is not an error: it means "no saved settings", and the caller keeps defaults.
        bool Load(RenderingSystem& rs, const std::filesystem::path& path);
        bool Save(const RenderingSystem& rs, const std::filesystem::path& path);

        // One "restirGi.halfResolution=0" style override, as taken by LuthShot's --set. Routed through
        // the same visitor by round-tripping the struct set through json, so every serialized field is
        // settable with no second mapping table to drift.
        //
        // Returns false for a malformed argument or an unknown key. Callers should treat that as fatal:
        // a typo in a sweep script must fail loudly, not silently measure the default and report it as
        // a result.
        bool ApplyOverride(RenderingSystem& rs, const std::string& assignment);
    }
}
