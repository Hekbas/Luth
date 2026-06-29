#include "luthpch.h"

#include "PhysicsMaterialImporter.h"

#include "luth/resources/AssetSerializer.h"

#include <fstream>

namespace Luth
{
    bool PhysicsMaterialImporter::Import(const std::filesystem::path& source, const std::filesystem::path& destination)
    {
        std::ifstream file(source);
        if (!file.is_open())
        {
            LH_LOG(Assets, error, "PhysicsMaterialImporter: Failed to open file {0}", source.string());
            return false;
        }

        PhysicsMaterialAssetData data;
        try {
            file >> data.JsonData;
        } catch (const std::exception& e) {
            LH_LOG(Assets, error, "PhysicsMaterialImporter: Failed to parse JSON {0}: {1}", source.string(), e.what());
            return false;
        }

        return AssetSerializer::SerializePhysicsMaterial(destination, data);
    }
}
