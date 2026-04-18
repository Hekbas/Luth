#pragma once

#include "luthien/Editor.h"

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
