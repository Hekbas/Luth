#include <Luth.h>
#include <Luth/core/EntryPoint.h>

#include <imgui.h>

namespace Luth
{
    class LuthienApp : public App
    {
    public:
        LuthienApp(int argc, char** argv) : App(argc, argv) {}
        ~LuthienApp() override = default;

    protected:
        void OnInit() override {}

        void OnUpdate() override
        {
        }

        void OnUIRender() override
        {
            // ImGui Demo
            static bool showDemo = true;
            if (showDemo) ImGui::ShowDemoWindow(&showDemo);

            ImGuiIO& io = ImGui::GetIO();
            ImGui::Begin("Luth Metrics");
            ImGui::Text("Frame time %.3f ms (%.1f FPS)", 1000.0f / io.Framerate, io.Framerate);
            ImGui::End();
        }

        void OnShutdown() override {}
    };

    App* CreateApp(int argc, char** argv)
    {
        return new LuthienApp(argc, argv);
    }
}
