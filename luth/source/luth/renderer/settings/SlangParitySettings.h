#pragma once

#include "luth/core/types/LuthTypes.h"

namespace Luth
{
    // Bindless-SPIR-V codegen regression gate verdict. A deterministic scan of the compiled
    // restir_gi_initial.slang SPIR-V (run when the guard initialises + on that shader's hot-reload): the
    // bindless rayQuery path must keep its NonUniform decorations and the capabilities that make them valid,
    // which slang#10525-class regressions drop or misplace. RenderPanel reads these read-only; the editor
    // never reaches the subsystem directly.
    struct SlangParitySettings
    {
        bool spirvChecked    = false;  // the scan has run at least once (guard initialised / shader reloaded)
        bool spirvPass       = true;   // capsOk && nonUniformCount > 0
        bool capsOk          = true;   // RuntimeDescriptorArray + PhysicalStorageBuffer + RayQuery + ShaderNonUniform
        u32  nonUniformCount = 0;      // OpDecorate NonUniform count on the bindless accesses (slang#10525 zeroes it)
    };
}
