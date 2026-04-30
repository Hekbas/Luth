#include "luthpch.h"
#include "luth/platform/Window.h"
#ifdef _WIN32
#include "luth/platform/WinWindow.h"
#else
#include "luth/platform/LinWindow.h"
#endif

namespace Luth
{
    std::unique_ptr<Window> Window::Create(const WindowSpec& spec)
    {
#ifdef _WIN32
        return std::make_unique<WinWindow>(spec);
#else
        return std::make_unique<LinWindow>(spec);
#endif
    }
}
