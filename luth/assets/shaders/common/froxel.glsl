// Froxel-atlas depth mapping shared by volumetric_composite.frag and the transparent-pass shaders.
// Slicing math replicated from volumetric_inject_density.comp — slices distribute logarithmically
// in view-Z (Wronski). Keep all sites in lockstep with the inject pass.

#ifndef LUTH_SHADERS_COMMON_FROXEL
#define LUTH_SHADERS_COMMON_FROXEL

// Inverse of glm::perspectiveRH_ZO (Vulkan depth range 0..1).
float FroxelDepthToViewZ(float ndcDepth, float nearZ, float farZ) {
    return (nearZ * farZ) / (ndcDepth * (nearZ - farZ) + farZ);
}

float FroxelViewZToSlice(float viewZ, float nearZ, float farZ) {
    return clamp(log(max(viewZ, nearZ) / nearZ) / log(farZ / nearZ), 0.0, 1.0);
}

#endif
