#pragma once

#include <cstdint>
#include <string>
#include <functional>
#include <iosfwd>

namespace Luth
{
    // 128-bit identity used for assets and entities. Generated from a Win32 entropy source on
    // construction; stable across save/load and across destroy-undo cycles, which is why every
    // command is keyed by UUID rather than entt::entity. Invalid() returns the all-zero sentinel.
    class UUID
    {
    public:
        UUID();
        UUID(uint64_t data0, uint64_t data1);
        UUID(const UUID&) = default;

        bool IsValid() const { return m_Data[0] != 0 || m_Data[1] != 0; }
        std::string ToString() const;

        // Static helpers
        static UUID Invalid() { return UUID(0, 0); }
        static UUID FromString(const std::string& string);
        
        // Accessors for hashing/internal use
        uint64_t GetHalf0() const { return m_Data[0]; }
        uint64_t GetHalf1() const { return m_Data[1]; }

        // Operators
        bool operator==(const UUID& other) const { return m_Data[0] == other.m_Data[0] && m_Data[1] == other.m_Data[1]; }
        bool operator!=(const UUID& other) const { return !(*this == other); }
        bool operator<(const UUID& other) const { return m_Data[0] < other.m_Data[0] || (m_Data[0] == other.m_Data[0] && m_Data[1] < other.m_Data[1]); }

        friend std::ostream& operator<<(std::ostream& os, const UUID& uuid);
        friend struct UUIDHash;

    private:
        uint64_t m_Data[2]; // 128-bit storage
    };

    inline std::ostream& operator<<(std::ostream& os, const UUID& uuid) {
        return os << uuid.ToString();
    }

    struct UUIDHash {
        size_t operator()(const UUID& uuid) const noexcept {
            // Combine hashes of both 64-bit parts
            uint64_t h1 = uuid.GetHalf0();
            uint64_t h2 = uuid.GetHalf1();
            return h1 ^ (h2 + 0x9e3779b9 + (h1 << 6) + (h1 >> 2));
        }
    };
}
