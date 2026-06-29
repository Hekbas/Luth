#include "lepch.h"
#include "luthien/EditorStyle.h"
#include "luthien/Editor.h"
#include "luth/resources/FileSystem.h"
#include "luthien/widgets/Icons.h"

#include <nlohmann/json.hpp>
#include <fstream>
#include <unordered_map>

namespace Luth::EditorStyle
{
    using json = nlohmann::json;

    // ── JSON helpers ──

    static json VecToJson(const ImVec2& v)  { return json::array({ v.x, v.y }); }
    static json VecToJson(const ImVec4& v)  { return json::array({ v.x, v.y, v.z, v.w }); }

    static ImVec2 JsonToVec2(const json& j, ImVec2 fallback = {})
    {
        if (!j.is_array() || j.size() < 2) return fallback;
        return { j[0].get<float>(), j[1].get<float>() };
    }
    static ImVec4 JsonToVec4(const json& j, ImVec4 fallback = {})
    {
        if (!j.is_array() || j.size() < 4) return fallback;
        return { j[0].get<float>(), j[1].get<float>(), j[2].get<float>(), j[3].get<float>() };
    }

    // Lookup: stable color-name → ImGuiCol index. ImGui::GetStyleColorName returns
    // e.g. "Text"/"WindowBg" — robust against enum reordering between ImGui versions.
    static const std::unordered_map<std::string, int>& ColorNameToIndex()
    {
        static const auto map = []() {
            std::unordered_map<std::string, int> m;
            for (int i = 0; i < ImGuiCol_COUNT; ++i)
                m.emplace(ImGui::GetStyleColorName(i), i);
            return m;
        }();
        return map;
    }

    static json Serialise(const StylePreset& p, const FontConfig& f)
    {
        json j;
        j["name"] = p.Name;

        j["font"] = {
            { "mainFont",           f.MainFontName },
            { "mainSize",           f.MainFontSize },
            { "mergeMainWithSolid", f.MergeMainWithSolid },
            { "iconSize",           f.IconFontSize },
        };

        j["dockingSeparatorSize"] = p.DockingSeparatorSize;

        j["windowRounding"]    = p.WindowRounding;
        j["childRounding"]     = p.ChildRounding;
        j["frameRounding"]     = p.FrameRounding;
        j["grabRounding"]      = p.GrabRounding;
        j["popupRounding"]     = p.PopupRounding;
        j["scrollbarRounding"] = p.ScrollbarRounding;
        j["tabRounding"]       = p.TabRounding;

        j["windowBorderSize"] = p.WindowBorderSize;
        j["childBorderSize"]  = p.ChildBorderSize;
        j["popupBorderSize"]  = p.PopupBorderSize;
        j["frameBorderSize"]  = p.FrameBorderSize;
        j["tabBorderSize"]    = p.TabBorderSize;

        j["windowPadding"]     = VecToJson(p.WindowPadding);
        j["framePadding"]      = VecToJson(p.FramePadding);
        j["itemSpacing"]       = VecToJson(p.ItemSpacing);
        j["itemInnerSpacing"]  = VecToJson(p.ItemInnerSpacing);
        j["touchExtraPadding"] = VecToJson(p.TouchExtraPadding);

        j["indentSpacing"] = p.IndentSpacing;
        j["scrollbarSize"] = p.ScrollbarSize;
        j["grabMinSize"]   = p.GrabMinSize;
        j["alpha"]         = p.Alpha;

        json colors = json::object();
        for (int i = 0; i < ImGuiCol_COUNT; ++i) {
            const ImVec4& c = p.Colors[i];
            // Skip fully-zero entries — factory presets leave unset colors at {0,0,0,0}
            if (c.x == 0 && c.y == 0 && c.z == 0 && c.w == 0) continue;
            colors[ImGui::GetStyleColorName(i)] = VecToJson(c);
        }
        j["colors"] = std::move(colors);

        return j;
    }

    static std::optional<StyleFile> Deserialise(const json& j)
    {
        StyleFile sf{};
        sf.Preset.Name = j.value("name", std::string("Unnamed"));

        if (auto it = j.find("font"); it != j.end() && it->is_object()) {
            sf.Font.MainFontName       = it->value("mainFont", std::string{});
            sf.Font.MainFontSize       = it->value("mainSize", 15.0f);
            sf.Font.MergeMainWithSolid = it->value("mergeMainWithSolid", false);
            sf.Font.IconFontSize       = it->value("iconSize", 14.0f);
        } else {
            LH_LOG(Editor, warn, "Style JSON missing 'font' block for '{}'", sf.Preset.Name);
            return std::nullopt;
        }

        auto& p = sf.Preset;
        p.DockingSeparatorSize = j.value("dockingSeparatorSize", 0.0f);

        p.WindowRounding    = j.value("windowRounding",    0.0f);
        p.ChildRounding     = j.value("childRounding",     0.0f);
        p.FrameRounding     = j.value("frameRounding",     0.0f);
        p.GrabRounding      = j.value("grabRounding",      0.0f);
        p.PopupRounding     = j.value("popupRounding",     0.0f);
        p.ScrollbarRounding = j.value("scrollbarRounding", 0.0f);
        p.TabRounding       = j.value("tabRounding",       0.0f);

        p.WindowBorderSize = j.value("windowBorderSize", 0.0f);
        p.ChildBorderSize  = j.value("childBorderSize",  0.0f);
        p.PopupBorderSize  = j.value("popupBorderSize",  0.0f);
        p.FrameBorderSize  = j.value("frameBorderSize",  0.0f);
        p.TabBorderSize    = j.value("tabBorderSize",    0.0f);

        p.WindowPadding     = JsonToVec2(j.value("windowPadding",     json::array()), {8,8});
        p.FramePadding      = JsonToVec2(j.value("framePadding",      json::array()), {6,4});
        p.ItemSpacing       = JsonToVec2(j.value("itemSpacing",       json::array()), {6,4});
        p.ItemInnerSpacing  = JsonToVec2(j.value("itemInnerSpacing",  json::array()), {4,4});
        p.TouchExtraPadding = JsonToVec2(j.value("touchExtraPadding", json::array()), {0,0});

        p.IndentSpacing = j.value("indentSpacing", 20.0f);
        p.ScrollbarSize = j.value("scrollbarSize", 14.0f);
        p.GrabMinSize   = j.value("grabMinSize",   12.0f);
        p.Alpha         = j.value("alpha",         1.0f);

        for (auto& c : p.Colors) c = { 0.0f, 0.0f, 0.0f, 0.0f };

        if (auto it = j.find("colors"); it != j.end() && it->is_object()) {
            const auto& nameMap = ColorNameToIndex();
            for (auto& [key, val] : it->items()) {
                auto m = nameMap.find(key);
                if (m == nameMap.end()) {
                    LH_LOG(Editor, warn, "Unknown ImGui color '{}' in style '{}'", key, sf.Preset.Name);
                    continue;
                }
                p.Colors[m->second] = JsonToVec4(val);
            }
        }

        return sf;
    }

    std::optional<StyleFile> LoadFromFile(const fs::path& path)
    {
        if (!fs::exists(path)) {
            LH_LOG(Editor, warn, "Style file not found: {}", path.string());
            return std::nullopt;
        }
        try {
            std::ifstream file(path);
            return Deserialise(json::parse(file));
        } catch (const std::exception& e) {
            LH_LOG(Editor, error, "Failed to load style '{}': {}", path.string(), e.what());
            return std::nullopt;
        }
    }

    bool SaveToFile(const StylePreset& preset, const FontConfig& font, const fs::path& path)
    {
        try {
            if (path.has_parent_path())
                fs::create_directories(path.parent_path());
            std::ofstream file(path);
            file << Serialise(preset, font).dump(4);
            LH_LOG(Editor, info, "Saved style '{}' to '{}'", preset.Name, path.string());
            return true;
        } catch (const std::exception& e) {
            LH_LOG(Editor, error, "Failed to save style '{}': {}", path.string(), e.what());
            return false;
        }
    }

    // ── Font Loading ──

    static ImFont* LoadIconFont(const char* filename, float size, bool mergeMode)
    {
        std::string path = (FileSystem::EngineAssetsPath("fonts") / filename).string();
        if (!fs::exists(path)) {
            LH_LOG(Editor, warn, "Icon font not found: {}", path);
            return nullptr;
        }

        ImFontConfig config;
        config.MergeMode = mergeMode;
        config.PixelSnapH = mergeMode;
        static const ImWchar iconRanges[] = { ICON_MIN_FA, ICON_MAX_FA, 0 };

        ImFont* font = ImGui::GetIO().Fonts->AddFontFromFileTTF(path.c_str(), size, &config, iconRanges);
        if (!font) {
            LH_LOG(Editor, warn, "Failed to load icon font: {}", path);
        }
        return font;
    }

    void LoadFonts(const FontConfig& config)
    {
        ImGuiIO& io = ImGui::GetIO();

        std::string mainPath = (FileSystem::EngineAssetsPath("fonts") / config.MainFontName).string();
        if (fs::exists(mainPath)) {
            ImFontConfig fontCfg;
            fontCfg.OversampleH = 3;
            fontCfg.OversampleV = 2;
            fontCfg.PixelSnapH  = true;
            Editor::MainFontRef() = io.Fonts->AddFontFromFileTTF(mainPath.c_str(), config.MainFontSize, &fontCfg);
        }
        else {
            LH_LOG(Editor, warn, "Main font not found: {}", mainPath);
        }

        if (config.MergeMainWithSolid) {
            // Custom/Rider: merge FA-Solid into main font, then add default + standalone FA-Regular
            Editor::FASolidRef() = LoadIconFont("fa-solid-900.ttf", config.IconFontSize, true);
            io.Fonts->AddFontDefault();
            Editor::FARegularRef() = LoadIconFont("fa-regular-400.ttf", config.IconFontSize, false);
        }
        else {
            // Bubblegum/Matrix: add default after main, standalone icon fonts
            io.Fonts->AddFontDefault();
            Editor::FARegularRef() = LoadIconFont("fa-regular-400.ttf", config.IconFontSize, false);
            Editor::FASolidRef() = LoadIconFont("fa-solid-900.ttf", config.IconFontSize, false);
        }

        // invariant: large 64-px bakes for ProjectPanel grid icons. Bilinear
        // downscale to small cells stays clean; upscale from the default 16-px
        // bake would not. Both Solid and Regular needed (empty folders use Regular).
        Editor::FASolidLargeRef()   = LoadIconFont("fa-solid-900.ttf",   64.0f, false);
        Editor::FARegularLargeRef() = LoadIconFont("fa-regular-400.ttf", 64.0f, false);
    }

    // ── Style Application ──

    void Apply(const StylePreset& preset)
    {
        ImGuiStyle& style = ImGui::GetStyle();

        style.DockingSeparatorSize = preset.DockingSeparatorSize;

        style.WindowRounding    = preset.WindowRounding;
        style.ChildRounding     = preset.ChildRounding;
        style.FrameRounding     = preset.FrameRounding;
        style.GrabRounding      = preset.GrabRounding;
        style.PopupRounding     = preset.PopupRounding;
        style.ScrollbarRounding = preset.ScrollbarRounding;
        style.TabRounding       = preset.TabRounding;

        style.WindowBorderSize  = preset.WindowBorderSize;
        style.ChildBorderSize   = preset.ChildBorderSize;
        style.PopupBorderSize   = preset.PopupBorderSize;
        style.FrameBorderSize   = preset.FrameBorderSize;
        style.TabBorderSize     = preset.TabBorderSize;

        style.WindowPadding     = preset.WindowPadding;
        style.FramePadding      = preset.FramePadding;
        style.ItemSpacing       = preset.ItemSpacing;
        style.ItemInnerSpacing  = preset.ItemInnerSpacing;
        style.TouchExtraPadding = preset.TouchExtraPadding;
        style.IndentSpacing     = preset.IndentSpacing;
        style.ScrollbarSize     = preset.ScrollbarSize;
        style.GrabMinSize       = preset.GrabMinSize;
        style.Alpha             = preset.Alpha;

        style.WindowMenuButtonPosition = ImGuiDir_None;

        memcpy(style.Colors, preset.Colors, sizeof(preset.Colors));
    }

    // ── Built-in loader ──

    std::optional<StyleFile> LoadBuiltin(const std::string& name)
    {
        return LoadFromFile(FileSystem::EngineAssetsPath("styles") / (name + ".json"));
    }
}
