#include <Luth.h>
#include <Luth/core/EntryPoint.h>

#include "OpenGLApp.h"
#include "VulkanApp.h"
#include "RTShaderApp.h"

#include <vector>
#include <string>
#include <cstring>
#include <utility>

namespace LuthTest
{
    void CreateTestArgs(const std::vector<std::string>& args, int& argc, char**& argv)
    {
        argc = static_cast<int>(args.size());
        argv = new char* [argc + 1];

        for (int i = 0; i < argc; ++i) {
            argv[i] = new char[args[i].size() + 1];
            std::strcpy(argv[i], args[i].c_str());
        }
        argv[argc] = nullptr;
    }

    void FreeTestArgs(int argc, char** argv)
    {
        for (int i = 0; i < argc; ++i) {
            delete[] argv[i];
        }
        delete[] argv;
    }
}

namespace Luth
{
    App* CreateApp(int argc, char** argv)
    {
        int testArgc = argc;
        char** testArgv = argv;

        if (argc < 2) { // No argument
            std::vector<std::string> args = { "Sandbox", "--opengl" };
            LuthTest::CreateTestArgs(args, testArgc, testArgv);

            LH_CORE_WARN("No arguments provided");
            LH_CORE_WARN("Valid Args: {} [--opengl|--vulkan|--rt]", argv[0]);
            LH_CORE_WARN("Initializing default {}", args[1]);
        }

        std::string appType = testArgv[1];
        if (appType == "--opengl") {
            return new OpenGLApp(testArgc, testArgv);
        }
        else if (appType == "--vulkan") {
            return new VulkanApp(testArgc, testArgv);
        }
        else if (appType == "--rt") {
            return new RTShaderApp(testArgc, testArgv);
        }
        else {  // Invalid argument
            std::vector<std::string> args = { "Sandbox", "--opengl" };
            LuthTest::CreateTestArgs(args, testArgc, testArgv);
            LH_CORE_WARN("Unknown argument: '{}'. Valid Args: [--opengl|--vulkan]", appType);
            LH_CORE_WARN("Initializing default [--opengl]");
            return new OpenGLApp(testArgc, testArgv);
        }
    }
}
