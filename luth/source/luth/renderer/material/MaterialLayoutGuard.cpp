#include "luthpch.h"
#include "luth/renderer/material/MaterialLayoutGuard.h"
#include "luth/renderer/shader/SlangCompiler.h"
#include "luth/core/diagnostics/Log.h"

namespace Luth::MaterialLayoutGuard
{
    bool Validate(const std::filesystem::path& slangPath, const char* typeName,
                  std::span<const CppField> cppFields, size_t cppSize)
    {
        SlangCompiler::StructLayout sl = SlangCompiler::ReflectStructLayout(slangPath, typeName);
        if (!sl.ok)
        {
            LH_CORE_WARN("MaterialLayoutGuard: reflection unavailable for '{}' — layout check skipped", typeName);
            return false;
        }

        bool match = (sl.size == cppSize);

        for (const CppField& cf : cppFields)
        {
            const SlangCompiler::StructLayout::Field* rf = nullptr;
            for (const auto& f : sl.fields)
                if (f.name == cf.name) { rf = &f; break; }

            if (!rf)
            {
                match = false;
                LH_CORE_ERROR("MaterialLayoutGuard: {}::{} missing from Slang reflection (C++ offset {})",
                              typeName, cf.name, cf.offset);
            }
            else if (rf->offset != cf.offset)
            {
                match = false;
                LH_CORE_ERROR("MaterialLayoutGuard: {}::{} offset drift — C++ {} vs Slang {}",
                              typeName, cf.name, cf.offset, rf->offset);
            }
        }

        if (!match)
        {
            LH_CORE_ERROR("MaterialLayoutGuard: {} LAYOUT DRIFT — C++ {} B/{} fields vs Slang {} B/{} fields",
                          typeName, cppSize, cppFields.size(), sl.size, sl.fields.size());
            assert(false && "MaterialLayoutGuard: C++/Slang struct layout drift — see log");
            return false;
        }

        LH_CORE_INFO("MaterialLayoutGuard: {} verified ({} fields, {} B)", typeName, cppFields.size(), sl.size);
        return true;
    }
}
