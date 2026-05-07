#pragma once

#include "luth/core/UUID.h"
#include "luth/resources/Asset.h"

#include <nlohmann/json.hpp>

namespace Luth
{
    // The .meta sidecar that pairs with each source asset to pin its UUID, type, per-type import
    // settings, and dependency list. AssetDatabase reads or creates one for every registered
    // asset; importers consult m_TypeSettings to drive things like texture wrap / filter modes.
    class MetaFile
    {
    public:
        explicit MetaFile(const UUID& uuid) : m_UUID(uuid) {}

        // Metadata operations
        static UUID Create(const fs::path& path, AssetType type);
        static void SetDefaultTypeSettings(AssetType type, MetaFile& meta);

        bool Load(const fs::path& metaPath);
        bool Save(const fs::path& metaPath) const;

        // Property access
        const UUID& GetUUID() const { return m_UUID; }
        nlohmann::json& GetTypeSettings() { return m_TypeSettings; }

        // Dependency tracking
        void AddDependency(const UUID& dependency);
        const std::vector<UUID>& GetDependencies() const { return m_Dependencies; }

    private:
        UUID m_UUID;
        std::vector<UUID> m_Dependencies;
        nlohmann::json m_TypeSettings;

        static constexpr int FORMAT_VERSION = 1;
    };
}
