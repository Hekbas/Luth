#pragma once

#include "luth/jobs/SpinLock.h"
#include <string>
#include <unordered_set>

namespace Luth
{
    // VK_NV_device_diagnostic_checkpoints marker registry. The driver treats the
    // marker as opaque, but we need to recover a human-readable name when dumping
    // checkpoints after a TDR. Names are interned once for the process lifetime —
    // leak by design; this is a diagnostic tool, not a steady-state primitive.
    // see arch/gpu-crash-debugging.md
    //
    // invariant: std::unordered_set keeps element addresses stable across insertion
    // (rehash relocates buckets, not elements), so c_str() of an interned string
    // never moves while the registry exists — the marker pointer stays resolvable.
    class GpuCheckpointRegistry
    {
    public:
        static const char* Intern(const std::string& name);
        static const char* Resolve(const void* marker);

    private:
        static SpinLock                        s_Lock;
        static std::unordered_set<std::string> s_Names;
    };
}
