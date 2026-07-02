#pragma once

#include "luth/core/types/LuthTypes.h"
#include "luth/platform/Window.h"
#include "luth/events/EventBus.h"
#include "luth/events/AppEvent.h"
#include "luth/events/FileDropEvent.h"
#include "luth/scene/Scene.h"
#include "luth/core/FrameData.h"
#include "luth/jobs/JobSystem.h"

#include <vector>
#include <memory>

namespace Luth
{
    // The engine's top-level entry point. Owns Window, FrameData, and JobSystem lifecycles, and drives
    // the pipelined Game(N) / Render(N-1) / GPU(N-2) frame loop. The main thread stays V2-isolated:
    // it pumps OS events, dispatches stage fibers, and presents; never enters the worker steal loop.
    // Luthien subclasses this to hook OnInit/OnUpdate/OnUIRender for the editor runtime.
    // See arch/frame-pipeline.md.
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

        // Load or switch to a project. Wires FileSystem, AssetDatabase, asset import, and
        // editor refresh in the right order; see LoadProject's body for the full sequence.
        void LoadProject(const std::filesystem::path& luthprojPath);

        // Stage entry points dispatched onto worker fibers from Run().
        // GameStage: Transform/Animation systems + CaptureSnapshot.
        // RenderStage: RenderingSystem record + submit.
        // args.data is the App* pointer.
        static void GameStageFn(JobSystem::JobArgs args);
        static void RenderStageFn(JobSystem::JobArgs args);

        std::shared_ptr<Window> m_Window;
        std::shared_ptr<Scene> m_Scene;

        // Frame Pipelining
        FrameData m_FrameData;

        bool m_Running = true;
        bool m_ProjectLoaded = false;

        // Per-frame flag set on main thread before dispatching the game stage.
        // Read by GameStageFn to gate AnimationSystem (paused, no-step, no-preview).
        bool m_RunGameSystems = true;
    };

    App* CreateApp(int argc, char** argv);
}
