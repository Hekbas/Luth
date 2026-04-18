#include "luthpch.h"
#include "luth/renderer/lighting/LightGatherer.h"
#include "luth/scene/Components.h"


namespace Luth
{
    void LightGatherer::Gather(entt::registry& registry,
                               LightUniforms& outLights,
                               DirectionalLightShadowParams& outShadow) const
    {
        outLights = {};

        // First-wins: only the first directional light contributes.
        bool foundDir = false;
        auto dirView = registry.view<Component::WorldTransform, Component::DirectionalLight>();
        for (auto [entity, wt, dl] : dirView.each())
        {
            if (foundDir) break;

            // Forward vector = -Z column of the world matrix
            outLights.dirLight.direction = Math::Normalize(-Vec3(wt.Matrix[2]));
            outLights.dirLight.color     = dl.Color;
            outLights.dirLight.intensity = dl.Intensity;

            outShadow.castShadows           = dl.CastShadows;
            outShadow.shadowBias            = Vec4(dl.ShadowBias[0], dl.ShadowBias[1], dl.ShadowBias[2], dl.ShadowBias[3]);
            outShadow.shadowNormalBias      = Vec4(dl.ShadowNormalBias[0], dl.ShadowNormalBias[1], dl.ShadowNormalBias[2], dl.ShadowNormalBias[3]);
            outShadow.splitLambda           = Math::Clamp(dl.SplitLambda, 0.0f, 1.0f);
            outShadow.shadowDistance        = dl.ShadowDistance;
            outShadow.stabilizeCascades     = dl.StabilizeCascades;
            outShadow.cascadeBlendWidth     = Math::Clamp(dl.CascadeBlendWidth, 0.0f, 1.0f);
            outShadow.debugVisualizeCascades = dl.DebugVisualizeCascades;
            foundDir = true;
        }

        if (!foundDir)
        {
            outLights.dirLight.direction = Math::Normalize(Vec3(1.0f, 1.0f, 0.5f));
            outLights.dirLight.color     = Vec3(1.0f);
            outLights.dirLight.intensity = 3.0f;
            // Shadow params: leave outShadow untouched so last-known config persists.
        }

        int count = 0;
        auto pointView = registry.view<Component::WorldTransform, Component::PointLight>();
        for (auto [entity, wt, pl] : pointView.each())
        {
            if (count >= 64) break;
            outLights.pointLights[count].position  = Vec3(wt.Matrix[3]);
            outLights.pointLights[count].color     = pl.Color;
            outLights.pointLights[count].intensity = pl.Intensity;
            outLights.pointLights[count].range     = pl.Range;
            ++count;
        }
        outLights.numPointLights = count;
    }
}
