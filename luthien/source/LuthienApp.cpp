#include <Luth.h>
#include <Luth/core/EntryPoint.h>

#include <imgui.h>

namespace Luth
{
    class LuthienApp : public App
    {
    public:
        LuthienApp() {}
        ~LuthienApp() override = default;

    protected:
        void OnInit() override {}

        void OnUpdate(f32 dt) override
        {
            static float time = 0;
            time += dt;
        }

        void OnUIRender() override
        {
            ImGui::Begin("Engine Dashboard");
            //ImGui::Text("FPS: %.1f", 1.0f / dt);
            ImGui::End();

            // ImGui Demo
            static bool showDemo = true;
            if (showDemo) {
                ImGui::ShowDemoWindow(&showDemo);
            }
        }

        void OnShutdown() override {}
    };

    App* CreateApp()
    {
        return new LuthienApp();
    }
}
