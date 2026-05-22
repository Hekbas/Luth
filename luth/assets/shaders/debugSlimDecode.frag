#version 450

// Slim G-buffer preview decoder for frame-debugger thumbnails.
// Handles float-sampled attachments via mode push constant:
//   mode 0 = SlimNormal (RG16F oct)        — decode to RGB world normal
//   mode 1 = SlimMotion (RG16F NDC delta)  — render abs(motion) * scale
//   mode 2 = SlimRoughness (R8)            — grayscale display
// SlimMaterialID (R16U) is integer-sampled and uses debugSlimMatID.frag.

layout(location = 0) in vec2 v_TexCoord;
layout(location = 0) out vec4 outColor;

layout(set = 0, binding = 0) uniform sampler2D u_Source;

layout(push_constant) uniform PushConstants {
    uint  mode;
    float scale;  // motion magnification (mode 1); unused otherwise
} pc;

// Karis 2014 octahedral decode inverse — input range [0, 1] → unit vec3.
vec3 OctDecode(vec2 enc)
{
    vec2 f = enc * 2.0 - 1.0;
    vec3 n = vec3(f.x, f.y, 1.0 - abs(f.x) - abs(f.y));
    float t = clamp(-n.z, 0.0, 1.0);
    n.x += n.x >= 0.0 ? -t : t;
    n.y += n.y >= 0.0 ? -t : t;
    return normalize(n);
}

void main()
{
    vec4 src = texture(u_Source, v_TexCoord);

    if (pc.mode == 0u)
    {
        // Octahedral world normal → RGB visualization (n * 0.5 + 0.5).
        vec3 N = OctDecode(src.rg);
        outColor = vec4(N * 0.5 + 0.5, 1.0);
    }
    else if (pc.mode == 1u)
    {
        // Motion vector (NDC delta) → |dx| in red, |dy| in green, scaled.
        // Default scale ~10-50 makes per-frame deltas visible at typical motion.
        vec2 mag = clamp(abs(src.rg) * pc.scale, 0.0, 1.0);
        outColor = vec4(mag.r, mag.g, 0.0, 1.0);
    }
    else
    {
        // Roughness (R8) → grayscale. R channel only; clamp to [0, 1] explicitly.
        float r = clamp(src.r, 0.0, 1.0);
        outColor = vec4(r, r, r, 1.0);
    }
}
