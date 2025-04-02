#pragma once

#include "luth/core/UUID.h"

namespace Luth
{
    enum class ResourceType {
        Model,
        Texture,
        Material,
        Shader,
        Font,
        Config,
        Directory,
        Unknown
    };

    class Resource
    {
    public:
        virtual ~Resource() = default;

        const UUID& GetUUID() const { return m_UUID; }
        void SetUUID(const UUID& uuid) { m_UUID = uuid; }

        const std::string& GetName() const { return m_Name; }
        void SetName(const std::string& name) { m_Name = name; }

    private:
        UUID m_UUID;
        std::string m_Name;
    };
}
