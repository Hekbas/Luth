#pragma once

#include "luthien/Editor.h"

namespace Luth
{
    // Undo / redo stack viewer. Reads CommandHistory directly; clicking an entry jumps the history
    // pointer to that point, replaying redo or undo as needed to land on the selection.
    struct HistorySnapshot { /* placeholder */ };

    class HistoryPanel : public Panel
    {
    public:
        HistoryPanel();
        void OnInit() override;
        void OnGather(EditorSnapshotBuilder& builder) override;
        void OnDraw(const EditorSnapshot& snapshot) override;
    };
}
