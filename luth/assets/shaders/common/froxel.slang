// Froxel-atlas depth mapping (Wronski log-Z slices) — twin of froxel.glsl. Keep in lockstep with the
// volumetric inject pass. see arch/rendering-pipeline.md

// Inverse of glm::perspectiveRH_ZO (Vulkan depth range 0..1).
public float FroxelDepthToViewZ(float ndcDepth, float nearZ, float farZ)
{
    return (nearZ * farZ) / (ndcDepth * (nearZ - farZ) + farZ);
}

public float FroxelViewZToSlice(float viewZ, float nearZ, float farZ)
{
    return clamp(log(max(viewZ, nearZ) / nearZ) / log(farZ / nearZ), 0.0, 1.0);
}
