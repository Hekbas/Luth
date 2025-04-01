#pragma once

#include <cstdint>
#include <random>

namespace Luth
{
    class UUID
    {
    public:
        UUID();
        explicit UUID(uint64_t uuid);

        std::string ToString() const;
        static bool FromString(const std::string& uuidString, UUID& outUUID);

        operator uint64_t() const { return m_UUID; }

        bool operator==(const UUID&) const = default;
        bool operator<(const UUID& other) const { return m_UUID < other.m_UUID; }

        friend std::ostream& operator<<(std::ostream& os, const UUID& uuid);

    private:
        uint64_t m_UUID;
    };

    inline std::ostream& operator<<(std::ostream& os, const UUID& uuid) {
        return os << uuid.ToString();
    }

    struct UUIDHash {
        size_t operator()(const UUID& uuid) const noexcept {
            return std::hash<uint64_t>{}(static_cast<uint64_t>(uuid));
        }
    };
}
