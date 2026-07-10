#pragma once

#include <filesystem>
#include <string>
#include <vector>
#include <initializer_list>

namespace fs = std::filesystem;

namespace Luth
{
    // Project-wide texture lookup, built once per model import. Backs the widened resolution strategies (project-wide filename/stem,
    // fuzzy) and convention auto-bind, so a referenced or convention-named texture resolves wherever it lives in the project
    // (parent, sibling, nested), not only under the model's own folder. Sources: a race-free AssetDatabase snapshot of registered
    // textures, plus a bounded scan of the model's own subtree that catches textures just copied in but not yet registered.
    class ProjectTextureIndex
    {
    public:
        struct Entry {
            fs::path    Path;
            std::string StemLower;      // filename without extension, lowercased
            std::string FilenameLower;  // filename with extension, lowercased
        };

        void Build(const fs::path& modelDir);

        // Exact filename (case-insensitive); the best candidate is the one nearest to modelDir. Empty if no match.
        fs::path FindByFilename(const std::string& filename, const fs::path& modelDir) const;

        // Stem (case-insensitive), extension-agnostic; the best candidate is the one nearest to modelDir. Empty if no match.
        fs::path FindByStem(const std::string& stem, const fs::path& modelDir) const;

        // Fuzzy stem-similarity above the confidence threshold; best is highest score, then nearest. outMatched receives the matched
        // stem for audit logging. Empty when nothing clears the threshold.
        fs::path FindFuzzy(const std::string& referenceStem, const fs::path& modelDir, std::string& outMatched) const;

        // Find a texture whose stem matches the material or model name AND carries one of the given role suffix tokens (case-insensitive).
        // Backs convention auto-bind of materials that arrive with no texture bindings.
        fs::path FindByConvention(const std::string& nameToken, const std::string& modelStem,
                                  std::initializer_list<const char*> suffixes,
                                  const fs::path& modelDir, std::string& outMatched) const;

        const std::vector<Entry>& Entries() const { return m_Entries; }
        bool Empty() const { return m_Entries.empty(); }

    private:
        void AddFile(const fs::path& p);
        std::vector<Entry> m_Entries;
    };
}
