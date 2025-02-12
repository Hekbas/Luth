#include "luthpch.h"
#include "luth/core/App.h"

namespace Luth
{
    void App::Run()
    {
        OnInit();

        while (m_Running)
        {
            OnUpdate();
            // timestep
            // events
        }

        OnShutdown();
    }

    void App::Close()
    {
        m_Running = false;
    }
}