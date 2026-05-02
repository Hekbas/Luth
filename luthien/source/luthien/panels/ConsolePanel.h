#pragma once

#include "luth/core/diagnostics/Log.h"
#include "luth/events/EventBus.h"
#include "luthien/Editor.h"

#include <deque>

namespace Luth
{
    struct ConsoleSnapshot { /* placeholder — filtering happens in OnDraw */ };

    // Editor-side log viewer. Implements both Panel and ILogSink: the sink callback
    // (any thread) re-publishes each entry as a LogEntrySignal on the main bus, the
    // panel's own subscription handler appends to m_Entries on main during
    // ProcessEvents — so OnGather/OnDraw never race with the writer.
    class ConsolePanel : public Panel, public ILogSink
    {
    public:
        ConsolePanel();
        ~ConsolePanel() override;

        void OnInit() override;
        void OnGather(EditorSnapshotBuilder& builder) override;
        void OnDraw(const EditorSnapshot& snapshot) override;
        void OnShutdown() override;

        void OnLogEntry(const LogEntry& entry) override;

    private:
        static constexpr size_t kCap = 1024;

        std::deque<LogEntry> m_Entries;
        SubscriptionHandle   m_LogSub;

        // Per-level visibility toggles. Trace / Debug off by default to keep the
        // steady-state stream readable (bursts from asset import are still in the
        // ring, just hidden until toggled).
        bool m_ShowTrace    = false;
        bool m_ShowDebug    = false;
        bool m_ShowInfo     = true;
        bool m_ShowWarn     = true;
        bool m_ShowError    = true;
        bool m_ShowCritical = true;

        char m_SearchBuf[128] = "";
        bool m_AutoScroll     = true;
        bool m_ScrollPending  = false;
    };
}
