// Slim G-buffer preview decoder (float attachments) for frame-debugger thumbnails.
//   mode 0 = SlimNormal (oct -> RGB normal); 1 = SlimMotion (|motion|*scale); 2 = SlimRoughness.
[[vk::binding(0, 0)]] Sampler2D u_Source;
struct PC { uint mode; float scale; };
[[vk::push_constant]] PC pc;
struct FIn { [[vk::location(0)]] float2 uv; };

float3 OctDecode(float2 enc)
{
    float2 f = enc * 2.0 - 1.0;
    float3 n = float3(f.x, f.y, 1.0 - abs(f.x) - abs(f.y));
    float t = clamp(-n.z, 0.0, 1.0);
    n.x += n.x >= 0.0 ? -t : t;
    n.y += n.y >= 0.0 ? -t : t;
    return normalize(n);
}

[shader("fragment")]
float4 main(FIn i) : SV_Target
{
    float4 src = u_Source.Sample(i.uv);
    if (pc.mode == 0u)
    {
        float3 N = OctDecode(src.rg);
        return float4(N * 0.5 + 0.5, 1.0);
    }
    else if (pc.mode == 1u)
    {
        float2 mag = clamp(abs(src.rg) * pc.scale, 0.0, 1.0);
        return float4(mag.r, mag.g, 0.0, 1.0);
    }
    else
    {
        float r = clamp(src.r, 0.0, 1.0);
        return float4(r, r, r, 1.0);
    }
}
