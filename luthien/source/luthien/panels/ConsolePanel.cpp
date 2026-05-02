#include "lepch.h"
#include "luthien/panels/ConsolePanel.h"

#include "luthien/EditorSnapshot.h"
#include "luthien/events/EditorSignals.h"
#include "luthien/widgets/Icons.h"

#include <imgui.h>

#include <chrono>
#include <cstring>
#include <ctime>

namespace Luth
{
    namespace {
        constexpr ImVec4 kColTrace    { 0.55f, 0.55f, 0.55f, 1.0f };
        constexpr ImVec4 kColDebug    { 0.55f, 0.65f, 0.85f, 1.0f };
        constexpr ImVec4 kColInfo     { 0.85f, 0.85f, 0.85f, 1.0f };
        constexpr ImVec4 kColWarn     { 0.95f, 0.78f, 0.30f, 1.0f };
        constexpr ImVec4 kColError    { 0.95f, 0.40f, 0.35f, 1.0f };
        constexpr ImVec4 kColCritical { 1.00f, 0.20f, 0.20f, 1.0f };

        const char* LevelIcon(LogLevel l)
        {
            switch (l) {
                case LogLevel::Trace:    return ICON_FA_BUG;
                case LogLevel::Debug:    return ICON_FA_BUG;
                case LogLevel::Info:     return ICON_FA_CIRCLE_INFO;
                case LogLevel::Warn:     return ICON_FA_TRIANGLE_EXCLAMATION;
                case LogLevel::Error:    return ICON_FA_CIRCLE_EXCLAMATION;
                case LogLevel::Critical: return ICON_FA_CIRCLE_XMARK;
                default:                 return "";
            }
        }

        const ImVec4& LevelColor(LogLevel l)
        {
            switch (l) {
                case LogLevel::Trace:    return kColTrace;
                case LogLevel::Debug:    return kColDebug;
                case LogLevel::Info:     return kColInfo;
                case LogLevel::Warn:     return kColWarn;
                case LogLevel::Error:    return kColError;
                case LogLevel::Critical: return kColCritical;
                default:                 return kColInfo;
            }
        }

        void FormatTimestamp(const std::chrono::system_clock::time_point& tp, char* out, size_t n)
        {
            const std::time_t t = std::chrono::system_clock::to_time_t(tp);
            std::tm tmv{};
            #ifdef _WIN32
                localtime_s(&tmv, &t);
            #else
                localtime_r(&t, &tmv);
            #endif
            std::strftime(out, n, "%H:%M:%S", &tmv);
        }

        bool ContainsCaseInsensitive(const std::string& haystack, const char* needle)
        {
            if (!needle || !*needle) return true;
            const size_t hl = haystack.size();
            const size_t nl = std::strlen(needle);
            if (nl > hl) return false;
            for (size_t i = 0; i + nl <= hl; ++i) {
                size_t j = 0;
                for (; j < nl; ++j) {
                    char a = haystack[i + j];
                    char b = needle[j];
                    if (a >= 'A' && a <= 'Z') a = char(a + ('a' - 'A'));
                    if (b >= 'A' && b <= 'Z') b = char(b + ('a' - 'A'));
                    if (a != b) break;
                }
                if (j == nl) return true;
            }
            return false;
        }

        // Unity-style level toggle: tinted icon button, click to toggle, tooltip
        // names the level. ImGui has no native toggle, so we re-skin Button via
        // the four state colours plus the text colour.
        void LevelToggle(const char* id, const char* icon, const char* tooltip,
                         const ImVec4& tint, bool& state)
        {
            constexpr float kBtnW = 28.0f;
            constexpr float kBtnH = 22.0f;

            const ImVec4 bgOff{ 0.16f, 0.16f, 0.16f, 1.0f };
            const ImVec4 bgOn { tint.x * 0.30f, tint.y * 0.30f, tint.z * 0.30f, 1.0f };
            const ImVec4 fgOn = tint;
            const ImVec4 fgOff{ 0.45f, 0.45f, 0.45f, 1.0f };

            ImGui::PushID(id);
            ImGui::PushStyleColor(ImGuiCol_Button,        state ? bgOn : bgOff);
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(state ? bgOn.x + 0.08f : 0.24f,
                                                                state ? bgOn.y + 0.08f : 0.24f,
                                                                state ? bgOn.z + 0.08f : 0.24f, 1.0f));
            ImGui::PushStyleColor(ImGuiCol_ButtonActive,  ImVec4(state ? bgOn.x + 0.15f : 0.30f,
                                                                state ? bgOn.y + 0.15f : 0.30f,
                                                                state ? bgOn.z + 0.15f : 0.30f, 1.0f));
            ImGui::PushStyleColor(ImGuiCol_Text,          state ? fgOn : fgOff);

            ImGui::PushFont(Editor::GetFASolid());
            if (ImGui::Button(icon, ImVec2(kBtnW, kBtnH))) state = !state;
            ImGui::PopFont();
            ImGui::PopStyleColor(4);
            ImGui::PopID();

            if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tooltip);
        }
    }

    ConsolePanel::ConsolePanel()
    {
        m_WindowID = "Console";
    }

    ConsolePanel::~ConsolePanel()
    {
        // RAII backstop in case Editor::Shutdown didn't reach OnShutdown for us
        // (e.g., crash mid-shutdown). RemoveSink + Unsubscribe are idempotent.
        Log::RemoveSink(this);
        EventBus::Unsubscribe(BusType::MainThread, m_LogSub);
    }

    void ConsolePanel::OnInit()
    {
        m_LogSub = EventBus::Subscribe<LogEntrySignal>(BusType::MainThread,
            [this](Event& e) {
                auto& sig = static_cast<LogEntrySignal&>(e);
                m_Entries.push_back(sig.GetEntry());
                if (m_Entries.size() > kCap) m_Entries.pop_front();
                if (m_AutoScroll) m_ScrollPending = true;
            });

        Log::AddSink(this);
    }

    void ConsolePanel::OnShutdown()
    {
        Log::RemoveSink(this);
        EventBus::Unsubscribe(BusType::MainThread, m_LogSub);
    }

    void ConsolePanel::OnLogEntry(const LogEntry& entry)
    {
        // Any-thread sink hop. Cross-thread safe via EventBus's queue mutex; main
        // drains during ProcessEvents (App.cpp, before Editor::Render).
        EventBus::Enqueue<LogEntrySignal>(BusType::MainThread, entry);
    }

    void ConsolePanel::OnGather(EditorSnapshotBuilder& builder)
    {
        builder.Add<ConsoleSnapshot>();
    }

    void ConsolePanel::OnDraw(const EditorSnapshot& /*snapshot*/)
    {
        LH_PROFILE_FUNCTION();
        if (!BeginWindow(ICON_FA_TERMINAL "  Console")) {
            ImGui::End();
            return;
        }

        // ── Toolbar row 1: Clear + Auto-scroll left, level toggles right ──
        if (ImGui::Button(ICON_FA_TRASH " Clear"))
            m_Entries.clear();
        ImGui::SameLine();
        ImGui::Checkbox("Auto-scroll", &m_AutoScroll);

        // Right-align level toggles. Cluster width = 6 buttons + 5 inter-spacings.
        constexpr int kLevels = 6;
        constexpr float kBtnW = 28.0f;
        const float spacing  = ImGui::GetStyle().ItemSpacing.x;
        const float clusterW = kLevels * kBtnW + (kLevels - 1) * spacing;
        const float rightX   = ImGui::GetWindowContentRegionMax().x - clusterW;
        ImGui::SameLine();
        if (ImGui::GetCursorPosX() < rightX) ImGui::SetCursorPosX(rightX);

        LevelToggle("##trace",    ICON_FA_BUG,                    "Trace",    kColTrace,    m_ShowTrace);    ImGui::SameLine();
        LevelToggle("##debug",    ICON_FA_BUG,                    "Debug",    kColDebug,    m_ShowDebug);    ImGui::SameLine();
        LevelToggle("##info",     ICON_FA_CIRCLE_INFO,            "Info",     kColInfo,     m_ShowInfo);     ImGui::SameLine();
        LevelToggle("##warn",     ICON_FA_TRIANGLE_EXCLAMATION,   "Warning",  kColWarn,     m_ShowWarn);     ImGui::SameLine();
        LevelToggle("##error",    ICON_FA_CIRCLE_EXCLAMATION,     "Error",    kColError,    m_ShowError);    ImGui::SameLine();
        LevelToggle("##critical", ICON_FA_CIRCLE_XMARK,           "Critical", kColCritical, m_ShowCritical);

        // ── Toolbar row 2: search ──
        ImGui::SetNextItemWidth(-1.0f);
        ImGui::InputTextWithHint("##search", ICON_FA_MAGNIFYING_GLASS "  Filter...",
                                 m_SearchBuf, sizeof(m_SearchBuf));

        ImGui::Separator();

        // ── List ──
        // No ImGuiListClipper: rows use TextWrapped (variable height) and per-frame
        // filter-skips, both of which break the clipper's first-row height probe.
        // ImGui still GPU-clips off-screen geometry; cost at cap=1024 is benign.
        if (ImGui::BeginChild("ConsoleList", ImVec2(0, 0), false,
                              ImGuiWindowFlags_HorizontalScrollbar))
        {
            for (size_t i = 0; i < m_Entries.size(); ++i) {
                const LogEntry& e = m_Entries[i];

                bool show = false;
                switch (e.level) {
                    case LogLevel::Trace:    show = m_ShowTrace;    break;
                    case LogLevel::Debug:    show = m_ShowDebug;    break;
                    case LogLevel::Info:     show = m_ShowInfo;     break;
                    case LogLevel::Warn:     show = m_ShowWarn;     break;
                    case LogLevel::Error:    show = m_ShowError;    break;
                    case LogLevel::Critical: show = m_ShowCritical; break;
                    default:                 show = true;           break;
                }
                if (!show) continue;
                if (m_SearchBuf[0] && !ContainsCaseInsensitive(e.message, m_SearchBuf))
                    continue;

                char ts[16];
                FormatTimestamp(e.timestamp, ts, sizeof(ts));

                const ImVec4& col = LevelColor(e.level);
                ImGui::PushStyleColor(ImGuiCol_Text, col);
                ImGui::TextDisabled("%s", ts);
                ImGui::SameLine();
                ImGui::PushFont(Editor::GetFASolid());
                ImGui::TextUnformatted(LevelIcon(e.level));
                ImGui::PopFont();
                ImGui::SameLine();
                ImGui::TextWrapped("%s", e.message.c_str());
                ImGui::PopStyleColor();
            }

            if (m_ScrollPending && m_AutoScroll) {
                ImGui::SetScrollHereY(1.0f);
                m_ScrollPending = false;
            }
        }
        ImGui::EndChild();

        ImGui::End();
    }
}
