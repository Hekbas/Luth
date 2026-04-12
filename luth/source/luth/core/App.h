#pragma once

#include "luth/core/LuthTypes.h"
#include "luth/platform/Window.h"
#include "luth/platform/EventBus.h"
#include "luth/platform/AppEvent.h"
#include "luth/platform/FileDropEvent.h"
#include "luth/scene/Scene.h"
#include "luth/renderer/FrameData.h"

#include <vector>
#include <memory>

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
        void SetAppTitle(WindowSpec& ws);

        void OnWindowResize(WindowResizeEvent& e);
        void OnWindowClose(WindowCloseEvent& e);
        void OnFileDrop(FileDropEvent& e);

        /// Load or switch to a project. Handles FileSystem, AssetDatabase, import, editor refresh.
        void LoadProject(const std::filesystem::path& luthprojPath);

        std::shared_ptr<Window> m_Window;
        std::shared_ptr<Scene> m_Scene;
        
        // Frame Pipelining
        FrameData m_FrameData;

        bool m_Running = true;
        bool m_ProjectLoaded = false;
    };

    App* CreateApp(int argc, char** argv);
}
