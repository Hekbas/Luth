#include "luthpch.h"
#include "luth/renderer/lighting/LightGatherer.h"
#include "luth/core/RenderSnapshot.h"


namespace Luth
{
    void LightGatherer::Gather(const RenderSnapshot& snapshot,
                               GatheredLights& outLights,
                               DirectionalLightShadowParams& outShadow) const
    {
        LH_PROFILE_FUNCTION();

        if (snapshot.directionalLight.present)
        {
            const auto& dl = snapshot.directionalLight;
            outLights.dirLight.direction = dl.direction;
            outLights.dirLight.color     = dl.color;
            outLights.dirLight.intensity = dl.intensity;
            outLights.dirLight._pad      = 0.0f;

            outShadow.castShadows           = dl.castShadows;
            outShadow.shadowBias            = dl.shadowBias;
            outShadow.shadowNormalBias      = dl.shadowNormalBias;
            outShadow.splitLambda           = dl.splitLambda;
            outShadow.shadowDistance        = dl.shadowDistance;
            outShadow.stabilizeCascades     = dl.stabilizeCascades;
            outShadow.cascadeBlendWidth     = dl.cascadeBlendWidth;
            outShadow.debugVisualizeCascades = dl.debugVisualizeCascades;
            outShadow.mode                  = dl.shadowing;
            outShadow.rtOriginEpsilon       = dl.rtOriginEpsilon;
            outShadow.rtNormalEpsilon       = dl.rtNormalEpsilon;
        }
        else
        {
            // No directional light in the scene: zero direct-sun contribution so only ambient/IBL
            // lights the surface, and disable shadows (castShadows gates the CSM/RT passes in
            // RenderPipeline). Direction/color are irrelevant at zero intensity but kept sane.
            outLights.dirLight.direction = Math::Normalize(Vec3(1.0f, 1.0f, 0.5f));
            outLights.dirLight.color     = Vec3(1.0f);
            outLights.dirLight.intensity = 0.0f;
            outLights.dirLight._pad      = 0.0f;
            outShadow.castShadows        = false;
        }

        const u32 count = static_cast<u32>(snapshot.pointLights.size());
        outLights.points.resize(count);
        for (u32 i = 0; i < count; ++i)
        {
            const auto& pl = snapshot.pointLights[i];
            outLights.points[i].position  = pl.position;
            outLights.points[i].range     = pl.range;
            outLights.points[i].color     = pl.color;
            outLights.points[i].intensity = pl.intensity;
        }

        const u32 spotCount = static_cast<u32>(snapshot.spotLights.size());
        outLights.spots.resize(spotCount);
        for (u32 i = 0; i < spotCount; ++i)
        {
            const auto& sl = snapshot.spotLights[i];
            // Clamp inner <= outer and keep cosInner strictly > cosOuter; the shader smoothstep
            // edges must not invert or collide (equal edges are undefined).
            const f32 outerDeg = Math::Clamp(sl.outerConeAngleDeg, 0.0f, 89.9f);
            const f32 innerDeg = Math::Clamp(sl.innerConeAngleDeg, 0.0f, outerDeg);
            auto& s = outLights.spots[i];
            s.position  = sl.position;
            s.range     = sl.range;
            s.direction = sl.direction;
            s.color     = sl.color;
            s.intensity = sl.intensity;
            s.cosOuter  = Math::Cos(Math::Radians(outerDeg));
            s.cosInner  = Math::Max(Math::Cos(Math::Radians(innerDeg)), s.cosOuter + 1e-3f);
            s._pad[0] = s._pad[1] = s._pad[2] = 0.0f;
        }
    }
}
