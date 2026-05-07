#pragma once

// Single-include main() forwarder. A runtime executable (Luthien.exe, future game runtimes) includes
// this header exactly once and provides Luth::CreateApp; the engine owns everything in between.

#include "luth/core/App.h"
#include "luth/core/diagnostics/Log.h"

#include <iostream>

int main(int argc, char** argv)
{
    Luth::Log::Init();
    Luth::App* app = Luth::CreateApp(argc, argv);

    if (!app) {
        LH_CORE_CRITICAL("Failed to create app. Exiting.");

        #if defined(LUTH_BUILD_DEBUG)
            std::cerr << "Press Enter to exit...";
            std::cin.ignore();
        #endif
    }

    app->Run();
    delete app;
}
