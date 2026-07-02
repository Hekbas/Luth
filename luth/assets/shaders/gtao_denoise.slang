// GTAO spatial denoise: 3x3 cross-bilateral on the raw AO, depth-weighted so silhouette edges don't bleed.
// Output is the half-res AO buffer the PBR shader samples. Standalone Set 0.
[[vk::binding(0, 0)]] Sampler2D u_RawAO;
[[vk::binding(1, 0)]] Sampler2D u_LinearDepth;
[[vk::binding(2, 0)]] [[vk::image_format("r8")]] RWTexture2D<float> u_FinalAO;

[shader("compute")]
[numthreads(8, 8, 1)]
void main(uint3 tid : SV_DispatchThreadID)
{
    int2 coord = int2(tid.xy);
    uint w, h; u_RawAO.GetDimensions(w, h);
    int2 size = int2(w, h);
    if (coord.x >= size.x || coord.y >= size.y) return;

    float2 invRes = 1.0 / float2(size);
    float2 uv     = (float2(coord) + 0.5) * invRes;

    float centerZ = u_LinearDepth.Sample(uv).r;

    float sum    = 0.0;
    float weight = 0.0;
    for (int dy = -1; dy <= 1; ++dy)
    for (int dx = -1; dx <= 1; ++dx)
    {
        float2 sampleUV = uv + float2(dx, dy) * invRes;
        float  sampleZ  = u_LinearDepth.Sample(sampleUV).r;
        float  sampleAO = u_RawAO.Sample(sampleUV).r;

        float relDZ = abs(sampleZ - centerZ) / max(centerZ, 0.01);
        float wz    = exp(-relDZ * relDZ * 100.0);
        float ws    = (dx == 0 && dy == 0) ? 1.0 : (dx == 0 || dy == 0) ? 0.5 : 0.25;
        float wgt   = wz * ws;

        sum    += sampleAO * wgt;
        weight += wgt;
    }

    u_FinalAO[coord] = weight > 0.0 ? sum / weight : 1.0;
}
