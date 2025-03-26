#include "luthpch.h"
#include "UUID.h"

namespace Luth
{
    UUID::UUID()
    {
        // Thread-safe initialization (C++11 guarantees)
        static thread_local std::random_device rd;
        static thread_local std::mt19937_64 engine(rd());
        static thread_local std::uniform_int_distribution<uint64_t> dist;

        m_UUID = dist(engine);
    }

    UUID::UUID(uint64_t uuid) : m_UUID(uuid) {}
}
