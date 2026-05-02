#include "luthpch.h"
#include "luth/core/diagnostics/StackTrace.h"
#include "luth/core/diagnostics/Log.h"

#ifdef _WIN32
    #include "luth/jobs/SpinLock.h"

    #define WIN32_LEAN_AND_MEAN
    #include <Windows.h>
    #include <DbgHelp.h>

    #include <cstdio>
    #include <mutex>
#endif

namespace Luth::StackTrace
{
#ifdef _WIN32
    namespace {
        std::once_flag s_InitOnce;
        SpinLock       s_SymLock;

        void EnsureInit()
        {
            std::call_once(s_InitOnce, []{
                SymSetOptions(SYMOPT_LOAD_LINES | SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS);
                SymInitialize(GetCurrentProcess(), nullptr, TRUE);
            });
        }
    }

    std::vector<std::string> Capture(int skip, int max)
    {
        EnsureInit();

        constexpr int kCaptureMax = 62;   // CaptureStackBackTrace per-call limit on x64
        if (max > kCaptureMax) max = kCaptureMax;
        if (max <= 0) return {};

        void* frames[kCaptureMax]{};
        const USHORT n = CaptureStackBackTrace((DWORD)(skip + 1), (DWORD)max, frames, nullptr);

        std::vector<std::string> out;
        out.reserve(n);

        // DbgHelp is single-threaded per MSDN; serialise resolution.
        SpinLockGuard guard(s_SymLock);

        const HANDLE proc = GetCurrentProcess();
        constexpr DWORD nameMax = 254;
        alignas(SYMBOL_INFO) char buf[sizeof(SYMBOL_INFO) + nameMax + 1]{};
        auto* sym = reinterpret_cast<SYMBOL_INFO*>(buf);
        sym->SizeOfStruct = sizeof(SYMBOL_INFO);
        sym->MaxNameLen   = nameMax;

        IMAGEHLP_LINE64 line{};
        line.SizeOfStruct = sizeof(line);

        for (USHORT i = 0; i < n; ++i) {
            const DWORD64 addr = reinterpret_cast<DWORD64>(frames[i]);
            DWORD disp = 0;
            const BOOL gotSym  = SymFromAddr(proc, addr, nullptr, sym);
            const BOOL gotLine = SymGetLineFromAddr64(proc, addr, &disp, &line);

            char fmt[768] = "";
            if (gotSym && gotLine) {
                std::snprintf(fmt, sizeof(fmt), "  %s (%s:%lu)",
                              sym->Name, line.FileName, (unsigned long)line.LineNumber);
            } else if (gotSym) {
                std::snprintf(fmt, sizeof(fmt), "  %s (0x%llx)",
                              sym->Name, (unsigned long long)addr);
            } else {
                std::snprintf(fmt, sizeof(fmt), "  0x%llx",
                              (unsigned long long)addr);
            }
            out.emplace_back(fmt);
        }
        return out;
    }

    void LogStackTrace(int skip, int max)
    {
        // skip + 1 to drop ourselves; caller's catch block becomes the top frame.
        auto frames = Capture(skip + 1, max);
        for (const auto& f : frames) {
            LH_CORE_ERROR("{}", f);
        }
    }
#else
    std::vector<std::string> Capture(int /*skip*/, int /*max*/) { return {}; }
    void LogStackTrace(int /*skip*/, int /*max*/) {}
#endif
}
