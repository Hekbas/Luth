#pragma once

#include "luth/core/LuthTypes.h"
#include "luth/window/Window.h"
#include "luth/events/EventBus.h"
#include "luth/events/AppEvent.h"
#include "luth/events/FileDropEvent.h"

#include <vector>

namespace Luth
{
    class App
    {
    public:
        App(int argc, char** argv);
        virtual ~App();

        void Run();
        void Close();

        WindowSpec ParseCommandLineArgs(int argc, char** argv);
        Window& GetWindow() { return *m_Window; }

    protected:
        virtual void OnInit() {}
        virtual void OnUpdate() {}
        virtual void OnUIRender() {}
        virtual void OnShutdown() {}

    private:
        void OnWindowResize(WindowResizeEvent& e);
        void OnWindowClose(WindowCloseEvent& e);
        void OnFileDrop(FileDropEvent& e);

    private:
        std::unique_ptr<Window> m_Window;
        std::shared_ptr<EventBus> m_MainThreadEventBus;

        bool m_Running = true;
        f32 m_LastFrameTime = 0.0f;
    };

    App* CreateApp(int argc, char** argv);
}
