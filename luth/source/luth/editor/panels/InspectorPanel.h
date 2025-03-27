#pragma once

#include "luth/editor/Editor.h"

namespace Luth
{
    class InspectorPanel : public Panel
    {
    public:
        void OnInit() override;
        void OnRender() override;
    };
}
