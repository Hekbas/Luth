#version 460

// Live slim G-buffer visualization — blits a decoded slim attachment to LDR for
// the editor's ShadeMode toggle. Replaces tonemap output when ShadeMode is
// SlimNormal / SlimRoughness / SlimMotion / SlimMaterialID. The frame-debugger
// preview path uses the same decode logic but runs on captured archives instead.

layout(location = 0) in vec2 v_TexCoord;
layout(location = 0) out vec4 outColor;

layout(set = 0, binding = 0) uniform sampler2D  u_SlimNormal;     // RG16F (oct)
layout(set = 0, binding = 1) uniform sampler2D  u_SlimRoughness;  // R8
layout(set = 0, binding = 2) uniform sampler2D  u_SlimMotion;     // RG16F (NDC delta)
layout(set = 0, binding = 3) uniform usampler2D u_SlimMaterialID; // R16U

layout(push_constant) uniform PushConstants {
    uint  mode;   // 0=normal, 1=roughness, 2=motion, 3=matID
    float scale;  // motion magnification (mode 2); unused otherwise
} pc;

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
    if (pc.mode == 0u)
    {
        vec3 N = OctDecode(texture(u_SlimNormal, v_TexCoord).rg);
        outColor = vec4(N * 0.5 + 0.5, 1.0);
    }
    else if (pc.mode == 1u)
    {
        float r = clamp(texture(u_SlimRoughness, v_TexCoord).r, 0.0, 1.0);
        outColor = vec4(r, r, r, 1.0);
    }
    else if (pc.mode == 2u)
    {
        vec2 m = texture(u_SlimMotion, v_TexCoord).rg;
        vec2 mag = clamp(abs(m) * pc.scale, 0.0, 1.0);
        outColor = vec4(mag.r, mag.g, 0.0, 1.0);
    }
    else
    {
        uint id = texture(u_SlimMaterialID, v_TexCoord).r;
        if (id == 0u) { outColor = vec4(0.0, 0.0, 0.0, 1.0); return; }
        float f = float(id);
        outColor = vec4(fract(f * 0.123), fract(f * 0.456), fract(f * 0.789), 1.0);
    }
}
