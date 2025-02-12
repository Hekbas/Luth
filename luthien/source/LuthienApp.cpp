#include "luth/core/App.h"

namespace Luth
{
    class LuthienApp : public App
    {
    public:
        LuthienApp() {}

        ~LuthienApp() override = default;

    protected:
        void OnInit() override {}

        void OnUpdate() override {}

        void OnShutdown() override {}
    };

    App* CreateApp()
    {
        return new LuthienApp();
    }
}

int main()
{
    Luth::App* app = Luth::CreateApp();
    app->Run();
    delete app;
    return 0;
}