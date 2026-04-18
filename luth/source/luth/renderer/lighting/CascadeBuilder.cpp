#include "luthpch.h"
#include "luth/renderer/lighting/CascadeBuilder.h"

#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <cmath>

namespace Luth
{
    void CascadeBuilder::ComputeSplits(float nearZ, float farZ, float lambda,
                                       float outFar[k_ShadowCascadeCount]) const
    {
        // Engel "Practical Split": lambda * Clog + (1-lambda) * Cuniform.
        const float ratio = farZ / std::max(nearZ, 1e-4f);
        for (u32 i = 0; i < k_ShadowCascadeCount; ++i)
        {
            float p    = float(i + 1) / float(k_ShadowCascadeCount);
            float clog = nearZ * std::pow(ratio, p);
            float cuni = nearZ + (farZ - nearZ) * p;
            outFar[i]  = lambda * clog + (1.0f - lambda) * cuni;
        }
    }

    glm::mat4 CascadeBuilder::ComputeMatrix(float nearD, float farD,
                                             const glm::vec3& lightDir,
                                             float tanHalfFovY, float aspect,
                                             const glm::mat4& camViewInv,
                                             bool stabilize,
                                             float& outWorldHalfExtent) const
    {
        // 8 corners of the sub-frustum slice [nearD, farD] in view space, then world space.
        const float hN = nearD * tanHalfFovY;
        const float wN = hN * aspect;
        const float hF = farD * tanHalfFovY;
        const float wF = hF * aspect;

        const glm::vec4 cornersVS[8] = {
            { -wN, -hN, -nearD, 1.0f }, {  wN, -hN, -nearD, 1.0f },
            {  wN,  hN, -nearD, 1.0f }, { -wN,  hN, -nearD, 1.0f },
            { -wF, -hF, -farD,  1.0f }, {  wF, -hF, -farD,  1.0f },
            {  wF,  hF, -farD,  1.0f }, { -wF,  hF, -farD,  1.0f },
        };

        glm::vec3 cornersWS[8];
        glm::vec3 center(0.0f);
        for (int i = 0; i < 8; ++i) {
            glm::vec4 w = camViewInv * cornersVS[i];
            cornersWS[i] = glm::vec3(w) / w.w;
            center += cornersWS[i];
        }
        center *= (1.0f / 8.0f);

        const glm::vec3 up = (glm::abs(glm::dot(lightDir, glm::vec3(0, 1, 0))) > 0.99f)
                             ? glm::vec3(1, 0, 0) : glm::vec3(0, 1, 0);

        // Direct port of Sascha Willems' updateCascades() from
        // https://github.com/SaschaWillems/Vulkan/blob/master/examples/shadowmappingcascade/shadowmappingcascade.cpp
        //
        // `stabilize` is intentionally unused: the fit is effectively always
        // stabilized via the 1/16-unit radius quantization below. Kept in the
        // signature to preserve callers and the serialized DirectionalLight flag.
        (void)stabilize;

        // Bounding sphere of the 8 slice corners, quantized to 1/16 unit so the slab
        // size is deterministic across frames (anti-shimmer).
        float radius = 0.0f;
        for (int i = 0; i < 8; ++i)
            radius = glm::max(radius, glm::length(cornersWS[i] - center));
        radius = std::ceil(radius * 16.0f) / 16.0f;

        outWorldHalfExtent = radius;

        // Eye placed exactly `radius` behind the frustum centroid along -lightDir.
        // Symmetric ortho covers lightView.z in [-2*radius, 0] → clip.z in [0, 1]
        // under GLM_FORCE_DEPTH_ZERO_TO_ONE (Luth's convention).
        //
        // No Y-flip: the shadow pass writes and pbr.frag samples through the same
        // matrix, so the pair is self-consistent regardless of NDC Y orientation.
        glm::mat4 lightView = glm::lookAt(center - lightDir * radius, center, up);
        glm::mat4 lightProj = glm::ortho(-radius, radius, -radius, radius, 0.0f, 2.0f * radius);
        return lightProj * lightView;
    }

    void CascadeBuilder::Build(const glm::vec3& lightDir,
                               const CameraParams& camera,
                               const DirectionalLightShadowParams& params,
                               CascadeData& out) const
    {
        // FOV / aspect recovered from the unflipped perspective projection.
        // projection[1][1] = 1/tan(fovY/2); projection[0][0] = 1/(aspect*tan(fovY/2)).
        const glm::mat4& proj = camera.projection;
        const float tanHalfFovY = (proj[1][1] != 0.0f) ? std::abs(1.0f / proj[1][1]) : 1.0f;
        const float aspect      = (proj[0][0] != 0.0f) ? std::abs(proj[1][1] / proj[0][0]) : 1.0f;
        const glm::mat4 camViewInv = glm::inverse(camera.view);

        const float nearZ = glm::max(camera.nearZ, 1e-3f);
        const float farZ  = glm::max(nearZ + 1e-3f,
                                     glm::min(camera.farZ, params.shadowDistance));

        float cascadeFar[k_ShadowCascadeCount];
        ComputeSplits(nearZ, farZ, params.splitLambda, cascadeFar);

        float cascadeNear = nearZ;
        for (u32 i = 0; i < k_ShadowCascadeCount; ++i)
        {
            const float cf = cascadeFar[i];
            float halfExtent = 1.0f;
            out.lightSpaceMatrix[i] = ComputeMatrix(
                cascadeNear, cf, lightDir, tanHalfFovY, aspect, camViewInv,
                params.stabilizeCascades, halfExtent);
            // World-space size of one shadow-map texel for this cascade.
            // Shader uses this to scale normal bias (expressed in texels) so a given
            // bias setting produces consistent offsets across cascades of different sizes.
            out.texelSize[i] = (2.0f * halfExtent) / float(k_ShadowResolution);
            cascadeNear = cf;
        }

        // GLSL-side cascade selection uses absolute view-Z distances (positive).
        out.splitsViewZ = glm::vec4(cascadeFar[0], cascadeFar[1], cascadeFar[2], cascadeFar[3]);
    }
}
