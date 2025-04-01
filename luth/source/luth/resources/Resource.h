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

    private:
        UUID m_UUID;
    };
}
