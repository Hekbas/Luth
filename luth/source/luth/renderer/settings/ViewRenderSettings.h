#pragma once

#include "luth/core/types/LuthTypes.h"

namespace Luth
{
    // Environment-lighting weights that reach the shader through CameraParams, not through a render
    // subsystem. They lived in Luthien's EditorSettings, which meant a headless capture could not see
    // them: LuthShot installs no EditorHooks, so EditorViewportState stayed default-constructed and
    // every offscreen frame rendered at iblIntensity 1.0 while the editor ran at whatever the user had
    // saved. That divergence silently invalidated any capture-vs-viewport comparison, so the fields
    // are engine-side render settings now and the editor edits these. see arch/rendering-pipeline.md
    struct ViewRenderSettings
    {
        f32  iblIntensity        = 1.0f;   // diffuse + specular IBL ambient weight
        f32  skyboxIntensity     = 1.0f;   // background skybox draw weight (does not light anything)
        bool enableVolumetricFog = true;
    };
}
