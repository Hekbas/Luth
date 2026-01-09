#include "luthpch.h"
#include "MaterialImporter.h"
#include <fstream>

namespace Luth
{
    bool MaterialImporter::Import(const std::filesystem::path& path, std::unique_ptr<AssetData>& outData)
    {
        std::ifstream file(path);
        if (!file.is_open())
        {
            LH_CORE_ERROR("MaterialImporter: Failed to open file {0}", path.string());
            return false;
        }

        auto matData = std::make_unique<MaterialAssetData>();
        try {
            file >> matData->JsonData;
        } catch (const std::exception& e) {
            LH_CORE_ERROR("MaterialImporter: Failed to parse JSON {0}: {1}", path.string(), e.what());
            return false;
        }

        outData = std::move(matData);
        return true;
    }
}