#include "luthpch.h"
#include "luth/renderer/lighting/LightGatherer.h"
#include "luth/core/RenderSnapshot.h"


namespace Luth
{
    void LightGatherer::Gather(const RenderSnapshot& snapshot,
                               LightUniforms& outLights,
                               DirectionalLightShadowParams& outShadow) const
    {
        outLights = {};

        if (snapshot.directionalLight.present)
        {
            const auto& dl = snapshot.directionalLight;
            outLights.dirLight.direction = dl.direction;
            outLights.dirLight.color     = dl.color;
            outLights.dirLight.intensity = dl.intensity;

            outShadow.castShadows           = dl.castShadows;
            outShadow.shadowBias            = dl.shadowBias;
            outShadow.shadowNormalBias      = dl.shadowNormalBias;
            outShadow.splitLambda           = dl.splitLambda;
            outShadow.shadowDistance        = dl.shadowDistance;
            outShadow.stabilizeCascades     = dl.stabilizeCascades;
            outShadow.cascadeBlendWidth     = dl.cascadeBlendWidth;
            outShadow.debugVisualizeCascades = dl.debugVisualizeCascades;
        }
        else
        {
            outLights.dirLight.direction = Math::Normalize(Vec3(1.0f, 1.0f, 0.5f));
            outLights.dirLight.color     = Vec3(1.0f);
            outLights.dirLight.intensity = 3.0f;
            // Shadow params: leave outShadow untouched so last-known config persists.
        }

        const u32 count = static_cast<u32>(snapshot.pointLights.size());
        for (u32 i = 0; i < count; ++i)
        {
            const auto& pl = snapshot.pointLights[i];
            outLights.pointLights[i].position  = pl.position;
            outLights.pointLights[i].color     = pl.color;
            outLights.pointLights[i].intensity = pl.intensity;
            outLights.pointLights[i].range     = pl.range;
        }
        outLights.numPointLights = static_cast<i32>(count);
    }
}
