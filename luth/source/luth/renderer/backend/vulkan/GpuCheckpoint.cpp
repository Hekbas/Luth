#include "luthpch.h"
#include "GpuCheckpoint.h"

namespace Luth
{
    SpinLock                        GpuCheckpointRegistry::s_Lock;
    std::unordered_set<std::string> GpuCheckpointRegistry::s_Names;

    const char* GpuCheckpointRegistry::Intern(const std::string& name)
    {
        SpinLockGuard guard(s_Lock);
        auto [it, _] = s_Names.insert(name);
        return it->c_str();
    }

    const char* GpuCheckpointRegistry::Resolve(const void* marker)
    {
        SpinLockGuard guard(s_Lock);
        for (const auto& s : s_Names)
            if (s.c_str() == marker) return s.c_str();
        return nullptr;
    }
}
