#pragma once

#include "luth/core/types/LuthTypes.h"

#include <filesystem>

namespace Luth
{
    // Scores a rendered image against a reference. Both inputs are LINEAR radiance, RGBA32F, same size.
    //
    // Linear, not tonemapped, on purpose: ACES crushes a 500-nit firefly to white in the test AND the
    // reference, so a comparison of display-encoded images measures near-zero error in exactly the place
    // the complaint lives. The PNG a capture also writes is for human eyes; these numbers come off the
    // HDR scene colour. see the arc notes in docs/development
    namespace ImageMetrics
    {
        struct Result
        {
            u32 width = 0, height = 0;

            // Rousselle relative MSE: mean of (t-r)^2 / (r^2 + eps). Normalising by reference intensity
            // stops bright regions from owning the score, which plain MSE cannot avoid in HDR.
            f64 relMSE = 0.0;
            f64 mse    = 0.0;
            f64 psnr   = 0.0;   // dB against the reference's own peak luminance

            // Fireflies are SPARSE outliers, so a mean flattens them. These two are the numbers that
            // actually track "it sparkles": the tail of the per-pixel relative-error distribution.
            f64 p99RelErr  = 0.0;
            f64 p999RelErr = 0.0;
            f64 maxRelErr  = 0.0;

            // SSIM over 8x8 windows on Reinhard-tonemapped luminance. Catches the opposite failure from
            // the percentiles: a denoiser that scores well on relMSE by smearing detail away.
            f64 ssim = 0.0;

            // Exposure sanity: a large ratio means the two images disagree on energy, and every other
            // number here is then measuring that offset rather than noise.
            f64 refMeanLum  = 0.0;
            f64 testMeanLum = 0.0;
        };

        // channels must be 4 (RGBA32F) for both buffers.
        Result Compare(const f32* test, const f32* ref, u32 w, u32 h);

        // False-colour per-pixel relative error, log-scaled so both a 5% bias and a 50x firefly are
        // visible in one image. Written as 8-bit PNG because it is for looking at, not measuring.
        bool WriteErrorMap(const std::filesystem::path& path,
                           const f32* test, const f32* ref, u32 w, u32 h);

        // Appends the result as a JSON object to the file at `path`, in the shape FrameBenchmark writes
        // so a sweep can parse both with one reader. Overwrites any existing file.
        bool WriteJson(const std::filesystem::path& path, const Result& r,
                       const std::filesystem::path& testPath,
                       const std::filesystem::path& refPath);
    }
}
