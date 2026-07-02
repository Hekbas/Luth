// SVGF (Schied 2017) shared helpers; twin of svgf_common.glsl. SvgfLinearizeDepth reads ubo.nearZ/farZ,
// so this imports globals. Intentionally independent of restir_common (OctDecode would collide); SVGF binds
// no light/reservoir state. see arch/rendering-pipeline.md
import globals;

public float SvgfLuminance(float3 c) { return dot(c, float3(0.2126, 0.7152, 0.0722)); }

// Octahedral decode: inverse of slim_gbuffer OctEncode (mirrors restir_common OctDecode).
public float3 SvgfOctDecode(float2 enc)
{
    enc = enc * 2.0 - 1.0;
    float3 n = float3(enc.xy, 1.0 - abs(enc.x) - abs(enc.y));
    float t = max(-n.z, 0.0);
    n.x += (n.x >= 0.0) ? -t : t;
    n.y += (n.y >= 0.0) ? -t : t;
    return normalize(n);
}

// Reversed-Z-agnostic linear eye depth from the hardware depth value.
public float SvgfLinearizeDepth(float d) { return (ubo.nearZ * ubo.farZ) / (ubo.farZ - d * (ubo.farZ - ubo.nearZ)); }
