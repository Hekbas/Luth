#include "luthpch.h"
#include "luth/renderer/backend/vulkan/AftermathCrashTracker.h"
#include "luth/core/diagnostics/Log.h"

#if defined(LUTH_ENABLE_AFTERMATH)

// Loads GFSDK_Aftermath_Lib.x64.dll at runtime (LoadLibrary, no link-time import) so a missing DLL
// soft-fails instead of aborting process startup. The SDK headers ship the PFN_ typedefs used here.
// see arch/gpu-crash-debugging.md
#include "GFSDK_Aftermath.h"
#include "GFSDK_Aftermath_GpuCrashDump.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>

#include <atomic>
#include <chrono>
#include <fstream>
#include <string>
#include <thread>

namespace Luth
{
    namespace
    {
        HMODULE          s_AftermathDll = nullptr;
        std::atomic<int> s_DumpCounter{ 0 };
        std::atomic<int> s_ShaderDbgCounter{ 0 };
        bool             s_Initialized = false;

        PFN_GFSDK_Aftermath_EnableGpuCrashDumps  s_EnableGpuCrashDumps  = nullptr;
        PFN_GFSDK_Aftermath_DisableGpuCrashDumps s_DisableGpuCrashDumps = nullptr;
        PFN_GFSDK_Aftermath_GetCrashDumpStatus   s_GetCrashDumpStatus   = nullptr;

        void WriteBlob(const std::string& path, const void* data, uint32_t size)
        {
            std::ofstream f(path, std::ios::binary);
            if (f) f.write(static_cast<const char*>(data), size);
        }

        // Driver invokes this on a TDR with the binary crash dump. Write it for Nsight Graphics.
        void GFSDK_AFTERMATH_CALL OnCrashDump(const void* dump, const uint32_t size, void*)
        {
            const std::string path = "luth_gpucrash_" + std::to_string(s_DumpCounter.fetch_add(1)) + ".nv-gpudmp";
            WriteBlob(path, dump, size);
            LH_CORE_CRITICAL("Aftermath: GPU crash dump written -> {} ({} bytes). Open it in Nsight Graphics.",
                             path, size);
        }

        // Per-shader debug info (deferred to crash time). Nsight matches these by embedded id, not name.
        void GFSDK_AFTERMATH_CALL OnShaderDebugInfo(const void* info, const uint32_t size, void*)
        {
            WriteBlob("luth_shaderdbg_" + std::to_string(s_ShaderDbgCounter.fetch_add(1)) + ".nvdbg", info, size);
        }

        void GFSDK_AFTERMATH_CALL OnDescription(PFN_GFSDK_Aftermath_AddGpuCrashDumpDescription addDesc, void*)
        {
            addDesc(GFSDK_Aftermath_GpuCrashDumpDescriptionKey_ApplicationName,    "Luthien");
            addDesc(GFSDK_Aftermath_GpuCrashDumpDescriptionKey_ApplicationVersion, "Luth");
        }

        // All-or-nothing resolve of the three entry points. Names are undecorated extern "C" on x64.
        bool LoadEntryPoints(HMODULE dll)
        {
            s_EnableGpuCrashDumps  = (PFN_GFSDK_Aftermath_EnableGpuCrashDumps) GetProcAddress(dll, "GFSDK_Aftermath_EnableGpuCrashDumps");
            s_DisableGpuCrashDumps = (PFN_GFSDK_Aftermath_DisableGpuCrashDumps)GetProcAddress(dll, "GFSDK_Aftermath_DisableGpuCrashDumps");
            s_GetCrashDumpStatus   = (PFN_GFSDK_Aftermath_GetCrashDumpStatus)  GetProcAddress(dll, "GFSDK_Aftermath_GetCrashDumpStatus");
            return s_EnableGpuCrashDumps && s_DisableGpuCrashDumps && s_GetCrashDumpStatus;
        }
    }

    void AftermathCrashTracker::Initialize()
    {
        // SDK path baked at build time first, then the exe directory. Hardened search flags keep a DLL
        // planted on the CWD / %PATH% from being loaded in its place.
    #if defined(LUTH_AFTERMATH_DLL)
        s_AftermathDll = LoadLibraryExA(LUTH_AFTERMATH_DLL, nullptr, LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
    #endif
        if (!s_AftermathDll)
            s_AftermathDll = LoadLibraryExA("GFSDK_Aftermath_Lib.x64.dll", nullptr,
                                            LOAD_LIBRARY_SEARCH_APPLICATION_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
        if (!s_AftermathDll)
        {
            LH_CORE_WARN("Aftermath: GFSDK_Aftermath_Lib.x64.dll not found - GPU crash dumps disabled "
                         "(copy it next to the exe or build with AFTERMATH_SDK set)");
            return;
        }
        if (!LoadEntryPoints(s_AftermathDll))
        {
            LH_CORE_WARN("Aftermath: DLL missing expected entry points (version mismatch?) - disabled");
            FreeLibrary(s_AftermathDll);
            s_AftermathDll = nullptr;
            return;
        }

        const GFSDK_Aftermath_Result r = s_EnableGpuCrashDumps(
            GFSDK_Aftermath_Version_API,
            GFSDK_Aftermath_GpuCrashDumpWatchedApiFlags_Vulkan,
            GFSDK_Aftermath_GpuCrashDumpFeatureFlags_DeferDebugInfoCallbacks,
            OnCrashDump, OnShaderDebugInfo, OnDescription, /*resolveMarkerCb*/ nullptr, nullptr);
        s_Initialized = GFSDK_Aftermath_SUCCEED(r);
        if (s_Initialized)
        {
            LH_CORE_INFO("Aftermath GPU crash dumps enabled");
        }
        else
        {
            LH_CORE_WARN("Aftermath: EnableGpuCrashDumps failed (0x{:x}) - disabled", static_cast<u32>(r));
            FreeLibrary(s_AftermathDll);
            s_AftermathDll = nullptr;
        }
    }

    void AftermathCrashTracker::OnDeviceLost()
    {
        if (!s_Initialized) return;
        LH_CORE_CRITICAL("Aftermath: device lost - collecting GPU crash dump (this can take a few seconds)...");

        // The dump is produced asynchronously on a driver thread; poll until it finishes. Bounded to
        // ~5s so a stuck collection never hangs shutdown. This is a crash path, so blocking is fine.
        GFSDK_Aftermath_CrashDump_Status status = GFSDK_Aftermath_CrashDump_Status_Unknown;
        s_GetCrashDumpStatus(&status);
        for (int i = 0; i < 100
                 && status != GFSDK_Aftermath_CrashDump_Status_CollectingDataFailed
                 && status != GFSDK_Aftermath_CrashDump_Status_Finished; ++i)
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            s_GetCrashDumpStatus(&status);
        }
        if (status != GFSDK_Aftermath_CrashDump_Status_Finished)
            LH_CORE_CRITICAL("Aftermath: crash dump did not finish (status={}).", static_cast<int>(status));
    }

    void AftermathCrashTracker::Shutdown()
    {
        if (s_Initialized) { s_DisableGpuCrashDumps(); s_Initialized = false; }
        if (s_AftermathDll) { FreeLibrary(s_AftermathDll); s_AftermathDll = nullptr; }
    }

    bool AftermathCrashTracker::Enabled() { return s_Initialized; }
}

#else // !LUTH_ENABLE_AFTERMATH — compiled-out no-ops so the engine builds without the SDK.

namespace Luth
{
    void AftermathCrashTracker::Initialize() {}
    void AftermathCrashTracker::OnDeviceLost() {}
    void AftermathCrashTracker::Shutdown() {}
    bool AftermathCrashTracker::Enabled() { return false; }
}

#endif
