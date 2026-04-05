#include "luthpch.h"
#include "luth/core/Version.h"

#include <cstdio>

namespace Luth
{
    static char s_VersionStr[32] = {};
    static char s_TitleStr[64]   = {};
    static bool s_Initialized    = false;

    static void InitStrings()
    {
        if (s_Initialized) return;

        if (VERSION_SUFFIX[0] != '\0')
            std::snprintf(s_VersionStr, sizeof(s_VersionStr),
                          "%u.%u.%u%s", VERSION_MAJOR, VERSION_MINOR, VERSION_PATCH, VERSION_SUFFIX);
        else
            std::snprintf(s_VersionStr, sizeof(s_VersionStr),
                          "%u.%u.%u", VERSION_MAJOR, VERSION_MINOR, VERSION_PATCH);

        std::snprintf(s_TitleStr, sizeof(s_TitleStr), "Luth %s [Vulkan]", s_VersionStr);
        s_Initialized = true;
    }

    const char* GetVersionString()  { InitStrings(); return s_VersionStr; }
    const char* GetFullTitleString() { InitStrings(); return s_TitleStr; }
}
