#pragma once

#include "luth/editor/Editor.h"

namespace Luth
{
    class HistoryPanel : public Panel
    {
    public:
        HistoryPanel();
        void OnInit() override;
        void OnRender() override;
    };
}
