#include "luthpch.h"
#include "luth/core/UUID.h"
#include "luth/jobs/JobSystem.h"

#include <random>
#include <cstring>

namespace Luth
{
    static std::random_device s_RandomDevice;
    static std::uniform_int_distribution<uint64_t> s_UniformDistribution;

    // Per-thread RNG pool indexed by JobContext::ThreadIndex (fiber-safe).
    static std::vector<std::mt19937_64> s_Engines;
    static std::mutex s_EngineInitLock;
    static bool s_EnginesInitialized = false;

    static void EnsureEnginesInitialized()
    {
        if (s_EnginesInitialized) return;
        std::lock_guard<std::mutex> lock(s_EngineInitLock);
        if (s_EnginesInitialized) return;

        s_Engines.resize(128);
        for (auto& engine : s_Engines)
        {
            engine.seed(s_RandomDevice());
        }
        s_EnginesInitialized = true;
    }

    static void GenerateFallback(uint64_t* data)
    {
        static std::mt19937_64 s_FallbackEngine(s_RandomDevice());
        static std::mutex s_FallbackLock;
        std::lock_guard<std::mutex> lock(s_FallbackLock);
        data[0] = s_UniformDistribution(s_FallbackEngine);
        data[1] = s_UniformDistribution(s_FallbackEngine);

        uint8_t* bytes = reinterpret_cast<uint8_t*>(data);
        bytes[6] = (bytes[6] & 0x0F) | 0x40;
        bytes[8] = (bytes[8] & 0x3F) | 0x80;
    }

    UUID::UUID()
    {
        EnsureEnginesInitialized();

        auto* ctx = JobSystem::GetCurrentJobContext();
        if (!ctx || s_Engines.empty())
        {
            GenerateFallback(m_Data);
            return;
        }

        u32 threadIndex = ctx->ThreadIndex;
        if (threadIndex >= s_Engines.size()) threadIndex = 0;

        std::mt19937_64& engine = s_Engines[threadIndex];

        m_Data[0] = s_UniformDistribution(engine);
        m_Data[1] = s_UniformDistribution(engine);

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
