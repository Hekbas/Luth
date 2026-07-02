// Live slim G-buffer visualization: decoded slim attachment to LDR for the editor ShadeMode toggle.
//   0 = normal, 1 = roughness, 2 = motion, 3 = matID. Standalone Set 0.
[[vk::binding(0, 0)]] Sampler2D       u_SlimNormal;      // RG16F oct
[[vk::binding(1, 0)]] Sampler2D       u_SlimRoughness;   // R8
[[vk::binding(2, 0)]] Sampler2D       u_SlimMotion;      // RG16F NDC delta
[[vk::binding(3, 0)]] Sampler2D<uint> u_SlimMaterialID;  // R16U

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
    if (pc.mode == 0u)
    {
        float3 N = OctDecode(u_SlimNormal.Sample(i.uv).rg);
        return float4(N * 0.5 + 0.5, 1.0);
    }
    else if (pc.mode == 1u)
    {
        float r = clamp(u_SlimRoughness.Sample(i.uv).r, 0.0, 1.0);
        return float4(r, r, r, 1.0);
    }
    else if (pc.mode == 2u)
    {
        float2 mag = clamp(abs(u_SlimMotion.Sample(i.uv).rg) * pc.scale, 0.0, 1.0);
        return float4(mag.r, mag.g, 0.0, 1.0);
    }
    else
    {
        uint id = u_SlimMaterialID.Sample(i.uv).r;
        if (id == 0u) return float4(0.0, 0.0, 0.0, 1.0);
        float f = float(id);
        return float4(frac(f * 0.123), frac(f * 0.456), frac(f * 0.789), 1.0);
    }
}
