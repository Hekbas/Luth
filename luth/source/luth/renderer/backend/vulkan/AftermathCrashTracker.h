#pragma once

// NVIDIA Nsight Aftermath GPU crash-dump tracker. On a device-lost (TDR) it writes a .nv-gpudmp
// (+ .nvdbg shader info) capturing the faulting shader, the page-fault address, and (with
// VK_NV_device_diagnostics_config resource tracking) the offending resource. This is the only
// post-mortem signal that survives a TDR and sees acceleration-structure / buffer-device-address
// faults, which synchronization validation cannot track and the basic checkpoint extension loses.
// see arch/gpu-crash-debugging.md
//
// No-op unless built with LUTH_ENABLE_AFTERMATH (premake defines it when the AFTERMATH_SDK env var
// points at the SDK). The DLL is loaded at runtime; a missing DLL soft-fails (disabled, no crash).
// Open the resulting .nv-gpudmp in Nsight Graphics to inspect.

namespace Luth
{
    class AftermathCrashTracker
    {
    public:
        // Enable crash dumps. Must run before vkCreateInstance / device creation.
        static void Initialize();

        // Called on VK_ERROR_DEVICE_LOST. Blocks (bounded) until the async crash dump is written.
        static void OnDeviceLost();

        // Disable crash dumps before instance/device teardown.
        static void Shutdown();

        static bool Enabled();
    };
}
