#include "luthpch.h"
#include "luth/core/UUID.h"

#include <random>
#include <cstring>

namespace Luth
{
    static std::random_device s_RandomDevice;
    static std::uniform_int_distribution<uint64_t> s_UniformDistribution;

    UUID::UUID()
    {
        // FIX: Use thread_local to make generation thread-safe without mutex locking overhead.
        // This creates a unique generator per thread, seeded once.
        static thread_local std::mt19937_64 s_Engine(s_RandomDevice());

        m_Data[0] = s_UniformDistribution(s_Engine);
        m_Data[1] = s_UniformDistribution(s_Engine);

        // Version 4, Variant 1 compliance
        // We safely cast to bytes to ensure we hit the specific bits required by RFC 4122
        // regardless of how uint64_t is laid out in registers.
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
