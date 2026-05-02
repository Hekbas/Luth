#pragma once

#include "luthien/Editor.h"

namespace Luth
{
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
