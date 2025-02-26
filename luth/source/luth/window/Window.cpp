#include "luthpch.h"
#include "luth/window/Window.h"
#include "luth/window/WinWindow.h"

namespace Luth
{
    std::unique_ptr<Window> Window::Create(const WindowSpec& spec)
    {
        return std::make_unique<WinWindow>(spec);
    }
}
