#pragma once

#include "luth/core/types/LuthMath.h"
#include "luth/renderer/CameraParams.h"
#include "luth/renderer/lighting/LightTypes.h"


namespace Luth
{
    // Builds per-cascade CSM data for a directional light:
    //   - PSSM split distances (Engel's practical-split weighted blend)
    //   - Per-cascade orthographic light-space matrix
    //     (Sascha Willems–style bounding-sphere fit with 1/16-unit quantisation)
    //   - World-space shadow-map texel size per cascade (shader scales normal bias)
    //
    // Stateless: safe to call once per frame.
    class CascadeBuilder
    {
    public:
        void Build(const Vec3& lightDir,
                   const CameraParams& camera,
                   const DirectionalLightShadowParams& params,
                   CascadeData& out) const;

    private:
        void ComputeSplits(float nearZ, float farZ, float lambda,
                           float outFar[k_ShadowCascadeCount]) const;

        // Returns the ortho light-space matrix for a single slice and writes
        // its world-space half-extent into outWorldHalfExtent.
        Mat4 ComputeMatrix(float nearD, float farD,
                                 const Vec3& lightDir,
                                 float tanHalfFovY, float aspect,
                                 const Mat4& camViewInv,
                                 bool stabilize,
                                 float& outWorldHalfExtent) const;
    };
}
