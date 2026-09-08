#include <Luth.h>
#include <Luth/core/EntryPoint.h>

namespace Luth
{
    // Offscreen capture runtime. Deliberately installs no EditorHooks: the engine's hook registry stays
    // empty, which is the headless path EditorHooks.h documents but nothing exercised until now.
    class LuthShotApp : public App
    {
    public:
        LuthShotApp(int argc, char** argv) : App(argc, argv) {}
        ~LuthShotApp() override = default;

    protected:
        void OnInit() override {}
        void OnUpdate() override {}
        void OnUIRender() override {}
        void OnShutdown() override {}
    };

    App* CreateApp(int argc, char** argv)
    {
        // Plain new to pair with EntryPoint's plain delete; LH_NEW would need a matching LH_DELETE.
        return new LuthShotApp(argc, argv);
    }
}
