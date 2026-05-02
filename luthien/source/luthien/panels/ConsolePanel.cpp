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
    }

    ConsolePanel::ConsolePanel()
    {
        m_WindowID = "Console";
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

        // ── Toolbar ──
        if (ImGui::Button(ICON_FA_TRASH " Clear"))
            m_Entries.clear();
        ImGui::SameLine();
        ImGui::Checkbox("Auto-scroll", &m_AutoScroll);

        ImGui::SameLine();
        ImGui::TextDisabled("|");
        ImGui::SameLine();
        ImGui::TextColored(kColTrace,    ICON_FA_BUG); ImGui::SameLine(); ImGui::Checkbox("Trc", &m_ShowTrace);     ImGui::SameLine();
        ImGui::TextColored(kColDebug,    ICON_FA_BUG); ImGui::SameLine(); ImGui::Checkbox("Dbg", &m_ShowDebug);     ImGui::SameLine();
        ImGui::TextColored(kColInfo,     ICON_FA_CIRCLE_INFO);          ImGui::SameLine(); ImGui::Checkbox("Inf", &m_ShowInfo);  ImGui::SameLine();
        ImGui::TextColored(kColWarn,     ICON_FA_TRIANGLE_EXCLAMATION); ImGui::SameLine(); ImGui::Checkbox("Wrn", &m_ShowWarn);  ImGui::SameLine();
        ImGui::TextColored(kColError,    ICON_FA_CIRCLE_EXCLAMATION);   ImGui::SameLine(); ImGui::Checkbox("Err", &m_ShowError); ImGui::SameLine();
        ImGui::TextColored(kColCritical, ICON_FA_CIRCLE_XMARK);         ImGui::SameLine(); ImGui::Checkbox("Crt", &m_ShowCritical);

        ImGui::SetNextItemWidth(-1.0f);
        ImGui::InputTextWithHint("##search", ICON_FA_MAGNIFYING_GLASS "  Filter...",
                                 m_SearchBuf, sizeof(m_SearchBuf));

        ImGui::Separator();

        // ── List ──
        const ImVec2 listSize{ 0.0f, 0.0f };
        if (ImGui::BeginChild("ConsoleList", listSize, false,
                              ImGuiWindowFlags_HorizontalScrollbar))
        {
            ImGuiListClipper clipper;
            clipper.Begin((int)m_Entries.size());
            while (clipper.Step()) {
                for (int i = clipper.DisplayStart; i < clipper.DisplayEnd; ++i) {
                    const LogEntry& e = m_Entries[(size_t)i];

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
