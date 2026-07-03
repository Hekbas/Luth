// Shared helpers for generated material-graph nodes. Imported CONDITIONALLY by generated modules (only
// when a graph uses a lib node) so pre-existing structures' canonical source stays hash-stable.
// invariant: everything here must be derivative-free and valid in fragment AND compute (raster == RT).

// 2D hash -> [0,1). Integer-free fract-style hash; stable enough for material noise, cheap everywhere.
float GraphHash2(float2 p)
{
    p = frac(p * float2(123.34, 345.45));
    p += dot(p, p + 34.345);
    return frac(p.x * p.y);
}

float GraphValueNoise(float2 p)
{
    float2 i = floor(p);
    float2 f = frac(p);
    float2 u = f * f * (3.0 - 2.0 * f);   // smoothstep fade
    float a = GraphHash2(i);
    float b = GraphHash2(i + float2(1.0, 0.0));
    float c = GraphHash2(i + float2(0.0, 1.0));
    float d = GraphHash2(i + float2(1.0, 1.0));
    return lerp(lerp(a, b, u.x), lerp(c, d, u.x), u.y);
}

// FBM value noise, normalized to [0,1]. params = the Noise node's data slot: (scale, octaves 1-4, -, -).
public float4 GraphNoise(float2 uv, float4 params)
{
    float2 p = uv * max(params.x, 1e-4);
    const int octaves = clamp(int(params.y), 1, 4);
    float amp = 0.5, sum = 0.0, norm = 0.0;
    for (int i = 0; i < octaves; ++i)
    {
        sum  += amp * GraphValueNoise(p);
        norm += amp;
        p *= 2.0;
        amp *= 0.5;
    }
    return float4(sum / max(norm, 1e-4));
}
