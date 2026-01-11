#include "luthpch.h"
#include "MaterialImporter.h"
#include "luth/resources/AssetSerializer.h"
#include <fstream>

namespace Luth
{
    bool MaterialImporter::Import(const std::filesystem::path& source, const std::filesystem::path& destination)
    {
        std::ifstream file(source);
        if (!file.is_open())
        {
            LH_CORE_ERROR("MaterialImporter: Failed to open file {0}", source.string());
            return false;
        }

        MaterialAssetData matData;
        try {
            file >> matData.JsonData;
        } catch (const std::exception& e) {
            LH_CORE_ERROR("MaterialImporter: Failed to parse JSON {0}: {1}", source.string(), e.what());
            return false;
        }

        return AssetSerializer::SerializeMaterial(destination, matData);
    }
}