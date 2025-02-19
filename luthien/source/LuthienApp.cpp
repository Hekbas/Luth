#include "Luth.h"

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
        void OnUpdate(f32 dt) override {}
        void OnUIRender() override {
            // Example ImGui window
            ImGui::Begin("Engine Dashboard");
            //ImGui::Text("FPS: %.1f", 1.0f / dt);
            ImGui::End();

            // Show demo window (for testing)
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

int main()
{
    Luth::Log::Init();
    Luth::App* app = Luth::CreateApp();
    app->Run();
    delete app;
    return 0;
}
