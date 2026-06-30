// Volumetric temporal-resolve pass. Reads this frame's post-integrate (scratch) atlas, reprojects each
// voxel center to the previous frame's resolved atlas via prevViewProjection + prev nearZ/farZ slice math,
// applies a Karis 3x3x3 min/max clamp from the 27 scratch neighbours to suppress disocclusion ghosting, and
// blends with temporalAlpha. Lives post-integrate so history + output share the integrated domain.
// see arch/rendering-pipeline.md
import globals;

[[vk::binding(0, 1)]] Sampler3D u_VolScratch;     // this frame's post-integrate
[[vk::binding(1, 1)]] Sampler3D u_VolPrev;        // prev frame's resolved (history)
[[vk::binding(2, 1)]] [[vk::image_format("rgba16f")]] RWTexture3D<float4> u_VolCurr;   // this frame's resolved (write)

struct PC
{
    float4x4 invView;
    uint     volDimX, volDimY, volDimZ, _pad;
};
[[vk::push_constant]] PC pc;

float SliceToViewZ(int i, float nearZ, float farZ)
{
    return nearZ * pow(farZ / nearZ, (float(i) + 0.5) / float(pc.volDimZ));
}
float DepthToViewZ(float ndcDepth, float nearZ, float farZ)
{
    return (nearZ * farZ) / (ndcDepth * (nearZ - farZ) + farZ);
}

[shader("compute")]
[numthreads(8, 8, 4)]
void main(uint3 tid : SV_DispatchThreadID)
{
    int3 volDim = int3(int(pc.volDimX), int(pc.volDimY), int(pc.volDimZ));
    int3 voxel  = int3(tid);
    if (any(voxel >= volDim)) return;

    // Voxel center → world position (mirrors inject's reconstruction).
    float2 uv    = (float2(voxel.xy) + 0.5) / float2(volDim.xy);
    float2 ndcXY = uv * 2.0 - 1.0;
    float  viewZ = SliceToViewZ(voxel.z, ubo.nearZ, ubo.farZ);
    float  vsZ   = -viewZ;
    float  vsX   = ndcXY.x * (-vsZ) / ubo.projection[0][0];
    float  vsY   = ndcXY.y * (-vsZ) / ubo.projection[1][1];
    float3 worldPos = mul(pc.invView, float4(float3(vsX, vsY, vsZ), 1.0)).xyz;

    float4 freshPost = u_VolScratch.Load(int4(voxel, 0));

    float4 prevClip = mul(ubo.prevViewProjection, float4(worldPos, 1.0));
    bool historyValid = (prevClip.w > 0.0)
        && all(prevClip.xy >= -prevClip.ww)
        && all(prevClip.xy <= prevClip.ww)
        && prevClip.z >= 0.0 && prevClip.z <= prevClip.w;

    float4 finalScatter = freshPost;
    if (historyValid)
    {
        float3 prevNDC = prevClip.xyz / prevClip.w;
        float2 prevUV  = prevNDC.xy * 0.5 + 0.5;
        float  prevNearZ = ubo.prevViewParams.x;
        float  prevFarZ  = ubo.prevViewParams.y;
        float  prevVZ    = DepthToViewZ(prevNDC.z, prevNearZ, prevFarZ);
        float  prevSliceW = clamp(log(max(prevVZ, prevNearZ) / prevNearZ) / log(prevFarZ / prevNearZ), 0.0, 1.0);

        float4 historySample = u_VolPrev.Sample(float3(prevUV, prevSliceW));

        // Karis 3x3x3 min/max clamp from scratch neighbours (RG barrier guarantees integrate wrote first).
        float4 lo = float4( 1e9);
        float4 hi = float4(-1e9);
        for (int dz = -1; dz <= 1; ++dz)
        for (int dy = -1; dy <= 1; ++dy)
        for (int dx = -1; dx <= 1; ++dx)
        {
            int3   n = clamp(voxel + int3(dx, dy, dz), int3(0), volDim - int3(1));
            float4 v = u_VolScratch.Load(int4(n, 0));
            lo = min(lo, v);
            hi = max(hi, v);
        }
        historySample = clamp(historySample, lo, hi);

        float alpha = ubo.volTemporalParams.y;
        finalScatter = lerp(historySample, freshPost, alpha);
    }

    u_VolCurr[voxel] = finalScatter;
}
