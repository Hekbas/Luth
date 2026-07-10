#include "luthpch.h"
#include "luth/resources/importers/ProjectTextureIndex.h"
#include "luth/resources/AssetDatabase.h"
#include "luth/resources/FileSystem.h"

#include <algorithm>
#include <cctype>
#include <unordered_set>

namespace Luth
{
    static constexpr int   k_ScanDepth      = 3;
    static constexpr float k_FuzzyThreshold = 0.6f;

    static const char* k_ImageExts[] = {
        ".png", ".jpg", ".jpeg", ".tga", ".bmp", ".tif", ".tiff", ".dds", ".hdr", ".exr", ".psd"
    };

    static std::string LowerAscii(std::string s)
    {
        std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        return s;
    }

    static bool IsImageExtLower(const std::string& extLower)
    {
        for (const char* e : k_ImageExts)
            if (extLower == e) return true;
        return false;
    }

    // Count of shared leading path components; a higher count means nearer. Compared on lexically-normal paths so a "a/b/../c" form
    // does not skew the count.
    static int CommonPrefix(const fs::path& a, const fs::path& b)
    {
        fs::path na = a.lexically_normal(), nb = b.lexically_normal();
        int n = 0;
        auto ia = na.begin(), ib = nb.begin();
        for (; ia != na.end() && ib != nb.end(); ++ia, ++ib) {
            if (*ia != *ib) break;
            ++n;
        }
        return n;
    }

    static std::vector<std::string> Tokens(const std::string& s)
    {
        std::vector<std::string> out;
        std::string cur;
        for (char c : s) {
            if (c == '_' || c == '-' || c == ' ' || c == '.') {
                if (!cur.empty()) { out.push_back(cur); cur.clear(); }
            } else {
                cur.push_back(c);
            }
        }
        if (!cur.empty()) out.push_back(cur);
        return out;
    }

    // Similarity in [0,1]: the max of substring-containment coverage and token Dice. Cheap, and tolerant of the T_<Name>_<suffix>
    // naming convention and of DCC export renames (for example a reference of "wunderbaum" against a file "T_Wunderbaum_BC").
    static float Similarity(const std::string& a, const std::string& b)
    {
        if (a.empty() || b.empty()) return 0.0f;

        float contain = 0.0f;
        if (b.find(a) != std::string::npos)      contain = static_cast<float>(a.size()) / b.size();
        else if (a.find(b) != std::string::npos) contain = static_cast<float>(b.size()) / a.size();

        std::vector<std::string> ta = Tokens(a), tb = Tokens(b);
        int shared = 0;
        for (const auto& x : ta)
            if (std::find(tb.begin(), tb.end(), x) != tb.end()) ++shared;
        float dice = (ta.empty() || tb.empty()) ? 0.0f : (2.0f * shared) / static_cast<float>(ta.size() + tb.size());

        return std::max(contain, dice);
    }

    void ProjectTextureIndex::AddFile(const fs::path& p)
    {
        const std::string extLower = LowerAscii(p.extension().string());
        if (!IsImageExtLower(extLower)) return;
        Entry e;
        e.Path          = p;
        e.StemLower     = LowerAscii(p.stem().string());
        e.FilenameLower = LowerAscii(p.filename().string());
        m_Entries.push_back(std::move(e));
    }

    void ProjectTextureIndex::Build(const fs::path& modelDir)
    {
        std::unordered_set<std::string> seen;
        auto tryAdd = [&](const fs::path& p) {
            std::error_code ec;
            std::string key = LowerAscii(fs::weakly_canonical(p, ec).string());
            if (ec) key = LowerAscii(p.string());
            if (seen.insert(key).second) AddFile(p);
        };

        // (a) Registered project textures: a race-free snapshot giving project-wide reach (parent, sibling, nested folders).
        for (const fs::path& p : AssetDatabase::GetPathsOfType(AssetType::Texture))
            tryAdd(p);

        // (b) Bounded scan of the model's own subtree: covers textures freshly copied in but not yet registered (drag-drop import).
        // Skips importer material/animation output dirs and *_baked artifacts, so only true source textures land in the index.
        if (fs::exists(modelDir)) {
            try {
                for (const auto& entry : fs::recursive_directory_iterator(modelDir, fs::directory_options::skip_permission_denied)) {
                    if (!entry.is_regular_file()) continue;

                    std::error_code ec;
                    fs::path rel = fs::relative(entry.path(), modelDir, ec);
                    int depth = 0;
                    for (auto it = rel.begin(); it != rel.end(); ++it) ++depth;
                    if (depth - 1 > k_ScanDepth) continue;

                    const std::string relLower = LowerAscii(rel.string());
                    if (relLower.find("_materials")  != std::string::npos) continue;
                    if (relLower.find("_animations") != std::string::npos) continue;
                    if (LowerAscii(entry.path().stem().string()).find("_baked") != std::string::npos) continue;

                    tryAdd(entry.path());
                }
            } catch (...) {}
        }
    }

    fs::path ProjectTextureIndex::FindByFilename(const std::string& filename, const fs::path& modelDir) const
    {
        const std::string fl = LowerAscii(filename);
        fs::path best;
        int bestScore = -1;
        for (const Entry& e : m_Entries) {
            if (e.FilenameLower != fl) continue;
            int s = CommonPrefix(e.Path, modelDir);
            if (s > bestScore) { bestScore = s; best = e.Path; }
        }
        return best;
    }

    fs::path ProjectTextureIndex::FindByStem(const std::string& stem, const fs::path& modelDir) const
    {
        const std::string sl = LowerAscii(stem);
        fs::path best;
        int bestScore = -1;
        for (const Entry& e : m_Entries) {
            if (e.StemLower != sl) continue;
            int s = CommonPrefix(e.Path, modelDir);
            if (s > bestScore) { bestScore = s; best = e.Path; }
        }
        return best;
    }

    fs::path ProjectTextureIndex::FindFuzzy(const std::string& referenceStem, const fs::path& modelDir,
                                            std::string& outMatched) const
    {
        const std::string ref = LowerAscii(referenceStem);
        fs::path best;
        float bestSim = 0.0f;
        int   bestNear = -1;
        for (const Entry& e : m_Entries) {
            float sim = Similarity(ref, e.StemLower);
            if (sim < k_FuzzyThreshold) continue;
            int nearScore = CommonPrefix(e.Path, modelDir);
            if (sim > bestSim || (sim == bestSim && nearScore > bestNear)) {
                bestSim = sim; bestNear = nearScore; best = e.Path; outMatched = e.StemLower;
            }
        }
        return best;
    }

    fs::path ProjectTextureIndex::FindByConvention(const std::string& nameToken, const std::string& modelStem,
                                                   std::initializer_list<const char*> suffixes,
                                                   const fs::path& modelDir, std::string& outMatched) const
    {
        const std::string name  = LowerAscii(nameToken);
        const std::string model = LowerAscii(modelStem);
        // Generic one or two character names (for example an unnamed slot) match far too much; require a token of real length.
        const bool useName  = name.size()  >= 3;
        const bool useModel = model.size() >= 3;
        if (!useName && !useModel) return {};

        fs::path best;
        int bestNear = -1;
        for (const Entry& e : m_Entries) {
            std::vector<std::string> toks = Tokens(e.StemLower);
            bool roleOk = false;
            for (const char* suf : suffixes)
                if (std::find(toks.begin(), toks.end(), LowerAscii(suf)) != toks.end()) { roleOk = true; break; }
            if (!roleOk) continue;

            bool nameOk = (useName  && e.StemLower.find(name)  != std::string::npos) ||
                          (useModel && e.StemLower.find(model) != std::string::npos);
            if (!nameOk) continue;

            int nearScore = CommonPrefix(e.Path, modelDir);
            if (nearScore > bestNear) { bestNear = nearScore; best = e.Path; outMatched = e.StemLower; }
        }
        return best;
    }
}
