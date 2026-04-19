#pragma once

#include <functional>

namespace Luth::UI
{
    bool BeginCollapsingHeader(const char* label, bool defaultOpen = false, const std::function<void()>& contextMenu = nullptr);
    void EndCollapsingHeader();
}
