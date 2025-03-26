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

        operator uint64_t() const { return m_UUID; }

        bool operator==(const UUID&) const = default;
        bool operator<(const UUID& other) const { return m_UUID < other.m_UUID; }

    private:
        uint64_t m_UUID;
    };
}
