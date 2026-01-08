#include <Luth.h>
#include <Luth/core/EntryPoint.h>

#include "VulkanApp.h"

namespace Luth
{
    App* CreateApp(int argc, char** argv)
    {
        return new VulkanApp(argc, argv);
    }
}
