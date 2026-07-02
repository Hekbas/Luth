// One-shot noise bake: writes a tile-able 128^3 RGBA8 3D texture used by the volumetric inject pass to add
// fog-density variation. Run once at VolumetricSubsystem::Init via ImmediateSubmit. Standalone Set 0.
//   R = low-freq Worley FBM, G = high-freq detail, B = mid-freq FBM, A = combined ready-to-use.
[[vk::binding(0, 0)]] [[vk::image_format("rgba8")]] RWTexture3D<float4> u_NoiseOut;

struct PC { uint dim; };   // 128 (atlas-side constant; keeps the shader resolution-agnostic)
[[vk::push_constant]] PC pc;

// GLSL mod (floor-based, sign of divisor); Slang fmod truncates, which breaks the tileable cell wrap.
float3 ModGlsl(float3 a, float b) { return a - b * floor(a / b); }

float3 hash33(float3 p)
{
    p = float3(dot(p, float3(127.1, 311.7, 74.7)),
               dot(p, float3(269.5, 183.3, 246.1)),
               dot(p, float3(113.5, 271.9, 124.6)));
    return frac(sin(p) * 43758.5453);
}

// Tile-able Worley by wrapping the integer cell coords modulo the tile dim before hashing.
float Worley3DTileable(float3 uvw, float cells)
{
    float3 p  = uvw * cells;
    float3 ip = floor(p);
    float3 fp = frac(p);
    float minDist2 = 1e6;
    for (int z = -1; z <= 1; ++z)
    for (int y = -1; y <= 1; ++y)
    for (int x = -1; x <= 1; ++x)
    {
        float3 cell    = ip + float3(x, y, z);
        float3 wrapped = ModGlsl(cell, cells);
        float3 featPos = float3(x, y, z) + hash33(wrapped);
        float3 r       = featPos - fp;
        minDist2       = min(minDist2, dot(r, r));
    }
    return clamp(sqrt(minDist2), 0.0, 1.0);
}

float WorleyInv(float3 uvw, float cells) { return 1.0 - Worley3DTileable(uvw, cells); }

float WorleyFBM(float3 uvw, float baseCells)
{
    float w = 0.0;
    float amp = 0.625;
    float cells = baseCells;
    for (int i = 0; i < 3; ++i)
    {
        w     += WorleyInv(uvw, cells) * amp;
        amp   *= 0.5;
        cells *= 2.0;
    }
    return clamp(w, 0.0, 1.0);
}

[shader("compute")]
[numthreads(4, 4, 4)]
void main(uint3 tid : SV_DispatchThreadID)
{
    int3 voxel = int3(tid);
    if (any(voxel >= int3(int(pc.dim)))) return;

    float3 uvw = (float3(voxel) + 0.5) / float(pc.dim);

    float low  = WorleyFBM(uvw, 4.0);     // 4 -> 8 -> 16 cells, large-scale cloud blobs
    float high = WorleyInv(uvw, 16.0);    // 16 cells, fine detail
    float mid  = WorleyFBM(uvw, 8.0);     // 8 -> 16 -> 32 cells, medium swirls

    float combined = clamp(lerp(low, mid, 0.5) - (high - 0.5) * 0.2, 0.0, 1.0);

    u_NoiseOut[voxel] = float4(low, high, mid, combined);
}
