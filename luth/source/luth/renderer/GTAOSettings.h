#pragma once

namespace Luth
{
    // GTAO (Ground Truth Ambient Occlusion, Jimenez 2016 / Intel XeGTAO)
    // runtime-tunable parameters. Mirrored to the GPU via GTAOUBO below
    // and read by gtao_main.comp / gtao_denoise.comp; intensity/enabled
    // are also sampled by pbr.frag when modulating the ambient term.
    struct GTAOSettings
    {
        bool  enabled        = true;
        bool  halfRes        = true;   // Output AO at half-res (XeGTAO default)
        bool  visualize      = false;  // Show raw AO buffer instead of lit color

        float intensity      = 1.0f;   // Overall AO strength multiplier (0 = off)
        float radius         = 0.5f;   // World-space sample radius (meters)
        float falloff        = 0.615f; // Horizon rejection distance (XeGTAO)
        float power          = 2.0f;   // pow(ao, power) for darkening curve

        int   sliceCount     = 3;      // Directional slices per pixel (2 / 3 / 4 / 8)
        int   stepsPerSlice  = 2;      // Samples along each horizon direction (1-6)
    };

    // std140-compatible UBO pushed every frame. Booleans widen to int because
    // std140 has no bool type. Padding is explicit so the C++ struct matches
    // the GLSL declaration byte-for-byte.
    struct GTAOUBO
    {
        float intensity;       // 0
        float radius;          // 4
        float falloff;         // 8
        float power;           // 12

        int   sliceCount;      // 16
        int   stepsPerSlice;   // 20
        int   enabled;         // 24
        int   visualize;       // 28

        float invResolution[2]; // 32 — 1.0 / half-res (or full-res) dimensions
        float invFullResolution[2]; // 40 — 1.0 / full-res dimensions (always full)
        // Total: 48 bytes
    };
}
