#pragma once

namespace Luth
{
    class App
    {
    public:
        App() = default;
        virtual ~App() = default;

        // Start the application loop
        void Run();

        // Signals the app to close
        void Close();

    protected:
        // Called once at startup
        virtual void OnInit() {}

        // Called every frame
        virtual void OnUpdate() {}

        // Called once on shutdown
        virtual void OnShutdown() {}

        bool m_Running = true;
    };

    App* CreateApp();
}