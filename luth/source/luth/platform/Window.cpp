#include "luthpch.h"
#include "luth/platform/Window.h"
#include "luth/platform/WinWindow.h"

namespace Luth
{
    std::unique_ptr<Window> Window::Create(const WindowSpec& spec)
    {
        return std::make_unique<WinWindow>(spec);
    }
}
