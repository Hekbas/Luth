#pragma once

#include <glm/glm.hpp>
#include "luth/renderer/settings/GTAOSettings.h"

namespace Luth
{
    enum class TonemapOperator : int {
        Linear     = 0,
        Reinhard   = 1,
        ACES       = 2,
        Uncharted2 = 3
    };

    struct PostProcessSettings
    {
        // Bloom
        float bloomThreshold  = 1.0f;
        float bloomStrength   = 0.5f;

        // Tone mapping
        TonemapOperator tonemapOp = TonemapOperator::ACES;
        float exposure   = 1.0f;
        float contrast   = 1.0f;
        float saturation = 1.0f;

        // Color balance
        glm::vec3 shadowBalance    = glm::vec3(1.0f);
        glm::vec3 midtoneBalance   = glm::vec3(1.0f);
        glm::vec3 highlightBalance = glm::vec3(1.0f);

        // Effects
        float vignetteAmount      = 0.0f;
        float vignetteHardness    = 0.5f;
        float grainAmount         = 0.0f;
        float sharpness           = 0.0f;
        float chromaticAberration = 0.0f;

        // Ambient occlusion (GTAO)
        GTAOSettings gtao;
    };

    // GPU-side UBO layout (std140 compatible, matches postprocess.frag)
    struct PostProcessUBO
    {
        float bloomThreshold;       // 0
        float bloomStrength;        // 4
        float exposure;             // 8
        float contrast;             // 12

        float saturation;           // 16
        int   tonemapOp;            // 20
        float vignetteAmount;       // 24
        float vignetteHardness;     // 28

        float grainAmount;          // 32
        float sharpness;            // 36
        float chromaticAberration;  // 40
        float time;                 // 44

        glm::vec3 shadowBalance;    float _pad0; // 48-60
        glm::vec3 midtoneBalance;   float _pad1; // 64-76
        glm::vec3 highlightBalance; float _pad2; // 80-92
        // Total: 96 bytes
    };
}
