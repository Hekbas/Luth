#pragma once

#include "luth/core/LuthTypes.h"
#include "luth/core/Window.h"
#include "luth/events/EventBus.h"

#include <vector>

namespace Luth
{
    class App
    {
    public:
        App();
        virtual ~App();

        void Run();
        void Close();

        // Event handling
        virtual void OnEvent(Event& event);

        // Window access
        Window& GetWindow() { return *m_Window; }

    protected:
        virtual void OnInit() {}
        virtual void OnUpdate(f32 dt) {}
        virtual void OnShutdown() {}

    private:
        void ProcessEvents();

        std::unique_ptr<Window> m_Window;
        EventBus m_EventBus;
        bool m_Running = true;
        f32 m_LastFrameTime = 0.0f;
    };

    App* CreateApp();
}
