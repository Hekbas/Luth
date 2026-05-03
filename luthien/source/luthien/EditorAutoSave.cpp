#include "lepch.h"
#include "luthien/EditorAutoSave.h"

#include "luth/core/EditorHooks.h"
#include "luth/core/time/Time.h"
#include "luth/events/EventBus.h"
#include "luth/jobs/IOThread.h"
#include "luth/jobs/MainThreadPump.h"
#include "luth/resources/FileSystem.h"
#include "luth/scene/Scene.h"
#include "luth/scene/SceneSerializer.h"

#include <imgui.h>

#include "luthien/Editor.h"
#include "luthien/EditorSettings.h"
#include "luthien/events/EditorSignals.h"

#include <algorithm>
#include <chrono>
#include <ctime>
#include <filesystem>
#include <vector>

namespace Luth
{
    namespace fs = std::filesystem;

    namespace {
        f32  s_LastSaveTime  = 0.0f;
        bool s_PlayActive    = false;
        f32  s_NoticeExpiry  = 0.0f;
        std::string         s_LastNotice;
        SubscriptionHandle  s_PlaySub;
        bool                s_WarnedUntitled    = false;
        bool                s_WarnedForeignPath = false;

        bool        s_RecoveryPending = false;
        bool        s_RecoveryModalOpened = false;
        fs::path    s_RecoveryFile;
        fs::path    s_RecoveryCanonicalPath;
        std::chrono::system_clock::time_point s_RecoveryStamp;

        constexpr f32 kNoticeDurationSec = 5.0f;

        fs::path AutosavesDir()
        {
            return FileSystem::ProjectPath() / ".luth" / "autosaves";
        }

        std::string FormatStamp(std::chrono::system_clock::time_point tp, bool withSeconds)
        {
            std::time_t t = std::chrono::system_clock::to_time_t(tp);
            std::tm tmv{};
        #ifdef _WIN32
            localtime_s(&tmv, &t);
        #else
            localtime_r(&t, &tmv);
        #endif
            char buf[32];
            std::strftime(buf, sizeof(buf),
                          withSeconds ? "%Y%m%d-%H%M%S" : "%H:%M",
                          &tmv);
            return buf;
        }

        bool IsPathInsideProject(const fs::path& p)
        {
            std::error_code ec;
            const auto root = fs::weakly_canonical(FileSystem::ProjectPath(), ec);
            const auto target = fs::weakly_canonical(p, ec);
            if (ec) return false;
            const auto& rootStr = root.native();
            const auto& targetStr = target.native();
            return targetStr.size() >= rootStr.size()
                && targetStr.compare(0, rootStr.size(), rootStr) == 0;
        }

        // invariant: SaveToString walks the EnTT registry and MUST run on main —
        // the registry is not race-safe vs main mutations. Only the file IO leg
        // hops to a worker thread via IOThread::WriteFile. See arch/fiber-system.md
        // V3 (cross-thread ECS). Prune is sequenced on the main pump callback so
        // it never overlaps the IOThread write that just produced the new file.
        void DispatchWrite()
        {
            auto scene = Editor::GetActiveScene();
            if (!scene) return;

            const fs::path scenePath = Editor::GetScenePath();
            const std::string stem = scenePath.stem().string();
            const auto now = std::chrono::system_clock::now();
            const std::string filename = stem + "-" + FormatStamp(now, true) + ".luth";
            const fs::path outPath = AutosavesDir() / filename;

            std::error_code ec;
            fs::create_directories(outPath.parent_path(), ec);
            if (ec) {
                LH_CORE_WARN("Autosave: failed to create '{}': {}",
                             outPath.parent_path().string(), ec.message());
                return;
            }

            std::string json = SceneSerializer::SaveToString(*scene);
            std::vector<u8> data(json.begin(), json.end());

            IOThread::WriteFile(outPath.string(), std::move(data));

            const std::string hhmm = FormatStamp(now, false);
            MainThreadPump::Post([hhmm]() {
                s_LastNotice   = "Autosaved " + hhmm;
                s_NoticeExpiry = Time::GetTime() + kNoticeDurationSec;

                // Prune oldest beyond keep-N. Cheap; same-thread-as-write
                // would race directory_iterator on Windows.
                const u32 keepN = Editor::GetSettings().autoSaveKeepN;
                if (keepN == 0) return;

                const fs::path scenePath = Editor::GetScenePath();
                const std::string stem = scenePath.stem().string() + "-";

                std::vector<fs::directory_entry> entries;
                std::error_code ec;
                for (const auto& entry : fs::directory_iterator(AutosavesDir(), ec)) {
                    if (!entry.is_regular_file()) continue;
                    const auto name = entry.path().filename().string();
                    if (name.rfind(stem, 0) != 0) continue;
                    entries.push_back(entry);
                }
                if (entries.size() <= keepN) return;

                std::sort(entries.begin(), entries.end(),
                    [](const fs::directory_entry& a, const fs::directory_entry& b) {
                        return a.last_write_time() < b.last_write_time();
                    });
                const size_t toDrop = entries.size() - keepN;
                for (size_t i = 0; i < toDrop; ++i) {
                    fs::remove(entries[i].path(), ec);
                }
            });
        }
    }

    void EditorAutoSave::Init()
    {
        s_LastSaveTime = Time::GetTime();
        s_PlayActive   = false;
        s_LastNotice.clear();
        s_NoticeExpiry = 0.0f;
        s_WarnedUntitled    = false;
        s_WarnedForeignPath = false;

        s_PlaySub = EventBus::Subscribe<PlayStateChangedSignal>(BusType::MainThread,
            [](Event& e) {
                auto& sig = static_cast<PlayStateChangedSignal&>(e);
                const bool nowPlaying = sig.GetTo() != PlayState::Editing;
                if (s_PlayActive && !nowPlaying) {
                    // Reset countdown so user gets a full interval after Stop.
                    s_LastSaveTime = Time::GetTime();
                }
                s_PlayActive = nowPlaying;
            });
    }

    void EditorAutoSave::Shutdown()
    {
        // invariant: shutdown does not autosave — racing engine teardown would
        // be worse than losing a final autosave window. User keeps manual Save.
        EventBus::Unsubscribe(BusType::MainThread, s_PlaySub);
        s_PlaySub = {};
    }

    void EditorAutoSave::Tick()
    {
        const auto& settings = Editor::GetSettings();
        if (!settings.autoSaveEnabled) return;
        if (s_PlayActive) return;
        if (!Editor::IsDirty()) return;

        auto scene = Editor::GetActiveScene();
        if (!scene) return;

        const fs::path scenePath = Editor::GetScenePath();
        if (scenePath.empty()) {
            if (!s_WarnedUntitled) {
                LH_CORE_WARN("Autosave skipped: scene has no path (Untitled).");
                s_WarnedUntitled = true;
            }
            return;
        }
        s_WarnedUntitled = false;

        if (!IsPathInsideProject(scenePath)) {
            if (!s_WarnedForeignPath) {
                LH_CORE_WARN("Autosave skipped: scene '{}' is outside project root.",
                             scenePath.string());
                s_WarnedForeignPath = true;
            }
            return;
        }
        s_WarnedForeignPath = false;

        if (Time::GetTime() - s_LastSaveTime < settings.autoSaveIntervalSec) return;

        DispatchWrite();
        s_LastSaveTime = Time::GetTime();
    }

    void EditorAutoSave::ForceNow()
    {
        // Same guards as Tick except interval — explicit user trigger.
        if (s_PlayActive) {
            LH_CORE_WARN("Autosave Now skipped: in Play mode.");
            return;
        }
        auto scene = Editor::GetActiveScene();
        if (!scene) {
            LH_CORE_WARN("Autosave Now skipped: no active scene.");
            return;
        }
        const fs::path scenePath = Editor::GetScenePath();
        if (scenePath.empty()) {
            LH_CORE_WARN("Autosave Now skipped: scene has no path.");
            return;
        }
        if (!IsPathInsideProject(scenePath)) {
            LH_CORE_WARN("Autosave Now skipped: scene outside project root.");
            return;
        }

        DispatchWrite();
        s_LastSaveTime = Time::GetTime();
    }

    const char* EditorAutoSave::GetLastNotice()
    {
        return IsNoticeActive() ? s_LastNotice.c_str() : "";
    }

    bool EditorAutoSave::IsNoticeActive()
    {
        return !s_LastNotice.empty() && Time::GetTime() < s_NoticeExpiry;
    }

    void EditorAutoSave::ScanForRecovery(const fs::path& scenePath)
    {
        s_RecoveryPending = false;
        s_RecoveryModalOpened = false;
        if (scenePath.empty()) return;
        if (!IsPathInsideProject(scenePath)) return;

        std::error_code ec;
        if (!fs::exists(scenePath, ec)) return;
        const auto canonStamp = fs::last_write_time(scenePath, ec);
        if (ec) return;

        const fs::path dir = AutosavesDir();
        if (!fs::exists(dir, ec)) return;

        const std::string stem = scenePath.stem().string() + "-";

        fs::path bestFile;
        fs::file_time_type bestStamp{};
        bool found = false;
        for (const auto& entry : fs::directory_iterator(dir, ec)) {
            if (!entry.is_regular_file()) continue;
            const auto name = entry.path().filename().string();
            if (name.rfind(stem, 0) != 0) continue;
            const auto stamp = entry.last_write_time();
            if (!found || stamp > bestStamp) {
                bestFile = entry.path();
                bestStamp = stamp;
                found = true;
            }
        }
        if (!found) return;
        if (bestStamp <= canonStamp) return;   // canonical is newer; autosave is stale

        s_RecoveryPending = true;
        s_RecoveryFile = bestFile;
        s_RecoveryCanonicalPath = scenePath;

        // file_time_type → system_clock::time_point conversion is implementation-
        // defined; clock_cast lands cleanly on MSVC C++20.
        s_RecoveryStamp = std::chrono::clock_cast<std::chrono::system_clock>(bestStamp);
    }

    void EditorAutoSave::DrawRecoveryModal()
    {
        if (!s_RecoveryPending) return;

        constexpr const char* kModalID = "Recover from autosave?";
        if (!s_RecoveryModalOpened) {
            ImGui::OpenPopup(kModalID);
            s_RecoveryModalOpened = true;
        }

        if (!ImGui::BeginPopupModal(kModalID, nullptr, ImGuiWindowFlags_AlwaysAutoResize))
            return;

        std::time_t t = std::chrono::system_clock::to_time_t(s_RecoveryStamp);
        std::tm tmv{};
    #ifdef _WIN32
        localtime_s(&tmv, &t);
    #else
        localtime_r(&t, &tmv);
    #endif
        char tsbuf[32];
        std::strftime(tsbuf, sizeof(tsbuf), "%Y-%m-%d %H:%M:%S", &tmv);

        ImGui::TextWrapped("A more recent autosave exists for '%s'.",
                           s_RecoveryCanonicalPath.filename().string().c_str());
        ImGui::Spacing();
        ImGui::TextDisabled("Autosave: %s", tsbuf);
        ImGui::TextDisabled("Path: %s", s_RecoveryFile.string().c_str());
        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();

        const ImVec2 btn{ 110, 0 };
        if (ImGui::Button("Recover", btn)) {
            auto scene = Editor::GetActiveScene();
            if (scene && SceneSerializer::Load(*scene, s_RecoveryFile)) {
                // The recovered content differs from the canonical file on disk;
                // mark dirty so the title-bar * is honest until the user Saves.
                Editor::MarkDirty();
                LH_CORE_INFO("Recovered scene from '{}'", s_RecoveryFile.string());
            }
            s_RecoveryPending = false;
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Discard", btn)) {
            std::error_code ec;
            fs::remove(s_RecoveryFile, ec);
            s_RecoveryPending = false;
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel", btn)) {
            s_RecoveryPending = false;
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
}
