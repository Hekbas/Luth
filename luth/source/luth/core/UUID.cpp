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

    std::string UUID::ToString() const
    {
        std::stringstream ss;
        ss << std::hex << std::setw(16) << std::setfill('0') << m_UUID;
        return ss.str();
    }

    bool UUID::FromString(const std::string& uuidString, UUID& outUUID)
    {
        if (uuidString.length() != 16) return false;

        for (char c : uuidString) {
            if (!std::isxdigit(c)) return false;
        }

        try {
            std::stringstream ss;
            ss << std::hex << uuidString;

            uint64_t result;
            if (ss >> result) {
                outUUID = UUID(result);
                return true;
            }
            return false;
        }
        catch (...) {
            return false;
        }
    }
}
