#include "luthpch.h"
#include "luth/tools/ImageMetrics.h"

#include "luth/resources/Image.h"

#include <algorithm>
#include <cmath>
#include <fstream>
#include <vector>

namespace Luth
{
    using json = nlohmann::json;

    namespace
    {
        constexpr f64 k_RelEps = 1.0e-2;   // Rousselle's denominator floor; keeps near-black pixels sane

        inline f64 Lum(const f32* p) { return 0.2126 * p[0] + 0.7152 * p[1] + 0.0722 * p[2]; }

        // Reinhard to [0,1] before SSIM: SSIM's constants assume a bounded dynamic range, so feeding it
        // raw HDR makes the luminance and contrast terms meaningless.
        inline f64 Tonemap(f64 v) { return v / (1.0 + v); }

        f64 Percentile(std::vector<f64>& v, f64 q)
        {
            if (v.empty()) return 0.0;
            const size_t k = static_cast<size_t>(q * static_cast<f64>(v.size() - 1));
            std::nth_element(v.begin(), v.begin() + k, v.end());
            return v[k];
        }

        // 8x8 windows, stride 4. Mean of per-window SSIM, the standard formulation with the usual
        // C1/C2 stabilisers for data scaled to [0,1].
        f64 Ssim(const std::vector<f64>& a, const std::vector<f64>& b, u32 w, u32 h)
        {
            constexpr f64 C1 = 0.01 * 0.01, C2 = 0.03 * 0.03;
            constexpr u32 W = 8, S = 4;
            if (w < W || h < W) return 0.0;

            f64 acc = 0.0; u32 n = 0;
            for (u32 y = 0; y + W <= h; y += S)
            {
                for (u32 x = 0; x + W <= w; x += S)
                {
                    f64 ma = 0.0, mb = 0.0;
                    for (u32 j = 0; j < W; ++j)
                        for (u32 i = 0; i < W; ++i)
                        { ma += a[(y + j) * w + x + i]; mb += b[(y + j) * w + x + i]; }
                    constexpr f64 inv = 1.0 / f64(W * W);
                    ma *= inv; mb *= inv;

                    f64 va = 0.0, vb = 0.0, cov = 0.0;
                    for (u32 j = 0; j < W; ++j)
                        for (u32 i = 0; i < W; ++i)
                        {
                            const f64 da = a[(y + j) * w + x + i] - ma;
                            const f64 db = b[(y + j) * w + x + i] - mb;
                            va += da * da; vb += db * db; cov += da * db;
                        }
                    va *= inv; vb *= inv; cov *= inv;

                    acc += ((2.0 * ma * mb + C1) * (2.0 * cov + C2))
                         / ((ma * ma + mb * mb + C1) * (va + vb + C2));
                    ++n;
                }
            }
            return (n > 0) ? acc / f64(n) : 0.0;
        }
    }

    namespace ImageMetrics
    {
        Result Compare(const f32* test, const f32* ref, u32 w, u32 h)
        {
            Result r;
            r.width = w; r.height = h;
            if (!test || !ref || w == 0 || h == 0) return r;

            const size_t n = static_cast<size_t>(w) * h;
            std::vector<f64> relErr; relErr.reserve(n);
            std::vector<f64> tmA(n), tmB(n);

            f64 sumRelMse = 0.0, sumMse = 0.0, sumRefLum = 0.0, sumTestLum = 0.0, peak = 0.0;

            for (size_t i = 0; i < n; ++i)
            {
                const f32* t = test + i * 4;
                const f32* s = ref  + i * 4;

                // Per-channel so a hue shift is not averaged away by the luminance weights.
                for (int c = 0; c < 3; ++c)
                {
                    const f64 d = f64(t[c]) - f64(s[c]);
                    sumMse    += d * d;
                    sumRelMse += (d * d) / (f64(s[c]) * f64(s[c]) + k_RelEps);
                }

                const f64 lt = Lum(t), ls = Lum(s);
                sumTestLum += lt; sumRefLum += ls;
                peak = std::max(peak, ls);

                const f64 e = std::abs(lt - ls) / (ls + k_RelEps);
                relErr.push_back(e);
                r.maxRelErr = std::max(r.maxRelErr, e);

                tmA[i] = Tonemap(lt);
                tmB[i] = Tonemap(ls);
            }

            const f64 nc = f64(n) * 3.0;
            r.mse    = sumMse / nc;
            r.relMSE = sumRelMse / nc;
            r.refMeanLum  = sumRefLum  / f64(n);
            r.testMeanLum = sumTestLum / f64(n);
            r.psnr = (r.mse > 0.0 && peak > 0.0) ? 10.0 * std::log10((peak * peak) / r.mse)
                                                 : std::numeric_limits<f64>::infinity();
            r.p99RelErr  = Percentile(relErr, 0.99);
            r.p999RelErr = Percentile(relErr, 0.999);
            r.ssim = Ssim(tmA, tmB, w, h);
            return r;
        }

        bool WriteErrorMap(const std::filesystem::path& path,
                           const f32* test, const f32* ref, u32 w, u32 h)
        {
            if (!test || !ref || w == 0 || h == 0) return false;
            const size_t n = static_cast<size_t>(w) * h;
            std::vector<u8> out(n * 4);

            for (size_t i = 0; i < n; ++i)
            {
                const f64 lt = Lum(test + i * 4), ls = Lum(ref + i * 4);
                const f64 e  = std::abs(lt - ls) / (ls + k_RelEps);

                // log10 mapped so 1% error is the bottom of the ramp and 10x is the top; one image then
                // shows a small systematic bias and a blown-out firefly at the same time.
                const f64 s = std::clamp((std::log10(std::max(e, 1.0e-3)) + 2.0) / 3.0, 0.0, 1.0);

                // Blue (match) -> green -> red (gross mismatch).
                f64 rr, gg, bb;
                if (s < 0.5) { const f64 u = s * 2.0;         rr = 0.0;     gg = u;         bb = 1.0 - u; }
                else         { const f64 u = (s - 0.5) * 2.0; rr = u;       gg = 1.0 - u;   bb = 0.0; }

                out[i * 4 + 0] = static_cast<u8>(std::clamp(rr, 0.0, 1.0) * 255.0);
                out[i * 4 + 1] = static_cast<u8>(std::clamp(gg, 0.0, 1.0) * 255.0);
                out[i * 4 + 2] = static_cast<u8>(std::clamp(bb, 0.0, 1.0) * 255.0);
                out[i * 4 + 3] = 255;
            }
            return Image::SavePng(path, out.data(), w, h, 4);
        }

        bool WriteJson(const std::filesystem::path& path, const Result& r,
                       const std::filesystem::path& testPath,
                       const std::filesystem::path& refPath)
        {
            json j;
            j["test"]   = testPath.string();
            j["ref"]    = refPath.string();
            j["width"]  = r.width;
            j["height"] = r.height;
            j["relMSE"] = r.relMSE;
            j["mse"]    = r.mse;
            j["psnr"]   = std::isfinite(r.psnr) ? json(r.psnr) : json("inf");
            j["p99RelErr"]   = r.p99RelErr;
            j["p999RelErr"]  = r.p999RelErr;
            j["maxRelErr"]   = r.maxRelErr;
            j["ssim"]        = r.ssim;
            j["refMeanLum"]  = r.refMeanLum;
            j["testMeanLum"] = r.testMeanLum;
            j["meanLumRatio"] = (r.refMeanLum > 0.0) ? (r.testMeanLum / r.refMeanLum) : 0.0;

            std::error_code ec;
            if (!path.parent_path().empty()) std::filesystem::create_directories(path.parent_path(), ec);
            std::ofstream f(path, std::ios::trunc);
            if (!f.is_open()) {
                LH_CORE_ERROR("ImageMetrics: cannot write '{}'", path.string());
                return false;
            }
            f << j.dump(4);
            return true;
        }
    }
}
