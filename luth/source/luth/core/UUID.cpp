#include "luthpch.h"
#include "luth/core/UUID.h"
#include "luth/core/JobSystem.h"

#include <random>
#include <cstring>

namespace Luth
{
    static std::random_device s_RandomDevice;
    static std::uniform_int_distribution<uint64_t> s_UniformDistribution;

    // We need a way to store the random engine in a fiber-safe way.
    // Since we don't have arbitrary data slots in JobContext yet, we can use a small pool of engines
    // indexed by ThreadIndex from JobContext.
    // Or, we can just use a spinlock for now since UUID generation isn't extremely hot in the render loop.
    // Actually, JobContext has ThreadIndex. We can use that.
    
    // Global pool of random engines, one per thread.
    // Max threads is usually small (e.g. 64).
    static std::vector<std::mt19937_64> s_Engines;
    static std::mutex s_EngineInitLock;
    static bool s_EnginesInitialized = false;

    static void EnsureEnginesInitialized()
    {
        if (s_EnginesInitialized) return;
        std::lock_guard<std::mutex> lock(s_EngineInitLock);
        if (s_EnginesInitialized) return;

        // Reserve enough for max threads. 
        // We can resize dynamically if needed, but std::vector resize is not thread safe.
        // Let's assume max 128 threads for now.
        s_Engines.resize(128);
        for (auto& engine : s_Engines)
        {
            engine.seed(s_RandomDevice());
        }
        s_EnginesInitialized = true;
    }

    UUID::UUID()
    {
        EnsureEnginesInitialized();

        // Get current thread index from JobSystem
        // If we are not in a job, we might be on the main thread or an external thread.
        // JobSystem::GetCurrentJobContext() returns nullptr if not in a fiber?
        // Actually, our JobSystem implementation sets s_CurrentFiber for Main Thread too.
        // So it should be safe.
        
        auto* ctx = JobSystem::GetCurrentJobContext();
        u32 threadIndex = 0;
        if (ctx)
        {
            threadIndex = ctx->ThreadIndex;
        }
        else
        {
            // Fallback for pre-init or external threads
            // Just use index 0 with a lock? Or hash thread ID?
            // For safety, let's just use a fallback engine with a lock.
            static std::mt19937_64 s_FallbackEngine(s_RandomDevice());
            static std::mutex s_FallbackLock;
            std::lock_guard<std::mutex> lock(s_FallbackLock);
            m_Data[0] = s_UniformDistribution(s_FallbackEngine);
            m_Data[1] = s_UniformDistribution(s_FallbackEngine);
            
            uint8_t* bytes = reinterpret_cast<uint8_t*>(m_Data);
            bytes[6] = (bytes[6] & 0x0F) | 0x40; 
            bytes[8] = (bytes[8] & 0x3F) | 0x80; 
            return;
        }

        if (threadIndex >= s_Engines.size()) threadIndex = 0; // Safety

        // No lock needed here because ThreadIndex is unique to the OS thread running this code.
        // Even if fibers migrate, they migrate *between* threads, so at any instant,
        // only one fiber is running on Thread K.
        std::mt19937_64& engine = s_Engines[threadIndex];
        
        m_Data[0] = s_UniformDistribution(engine);
        m_Data[1] = s_UniformDistribution(engine);

        // Version 4, Variant 1 compliance
        uint8_t* bytes = reinterpret_cast<uint8_t*>(m_Data);
        bytes[6] = (bytes[6] & 0x0F) | 0x40; // Version 4
        bytes[8] = (bytes[8] & 0x3F) | 0x80; // Variant 1
    }

    UUID::UUID(uint64_t data0, uint64_t data1)
    {
        m_Data[0] = data0;
        m_Data[1] = data1;
    }

    // High-performance hex lookup table
    static const char s_HexChars[] = "0123456789abcdef";

    std::string UUID::ToString() const
    {
        // Format: xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx (36 chars)
        std::string str(36, '0');
        
        const uint8_t* bytes = reinterpret_cast<const uint8_t*>(m_Data);

        int strIdx = 0;
        for (int i = 0; i < 16; ++i)
        {
            str[strIdx++] = s_HexChars[(bytes[i] >> 4) & 0xF];
            str[strIdx++] = s_HexChars[bytes[i] & 0xF];

            if (i == 3 || i == 5 || i == 7 || i == 9)
            {
                str[strIdx++] = '-';
            }
        }

        return str;
    }

    // Helper to convert a single hex char to uint8_t
    static uint8_t HexCharToInt(char c)
    {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return 0;
    }

    UUID UUID::FromString(const std::string& string)
    {
        if (string.length() != 36) return UUID(0, 0);

        uint64_t data[2] = { 0 };
        uint8_t* bytes = reinterpret_cast<uint8_t*>(data);

        int byteIdx = 0;
        for (size_t i = 0; i < string.length(); ++i)
        {
            if (string[i] == '-') continue;

            if (byteIdx >= 16) break; // Safety break

            char high = string[i];
            char low = string[++i]; // Consuming two chars per byte

            bytes[byteIdx++] = (HexCharToInt(high) << 4) | HexCharToInt(low);
        }

        return UUID(data[0], data[1]);
    }
}
