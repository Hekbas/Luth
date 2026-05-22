#version 460
#extension GL_EXT_nonuniform_qualifier : enable

// Samples the engine-wide bindless texture array. Caller supplies the material's
// diffuse bindless index in the push constant; slot 0 is the reserved 1x1 white
// (used for mesh bakes and as the fallback when a material has no diffuse map).

layout(set = 0, binding = 0) uniform sampler2D globalTextures[];

layout(push_constant) uniform PC {
    mat4 viewProj;
    vec4 albedo;
    uint diffuseIndex;
} pc;

layout(location = 0) in vec3 vNormal;
layout(location = 1) in vec4 vAlbedo;
layout(location = 2) in vec2 vUV;

layout(location = 0) out vec4 outColor;

void main()
{
    vec4 sampled  = texture(globalTextures[nonuniformEXT(pc.diffuseIndex)], vUV);
    vec3 base     = sampled.rgb * vAlbedo.rgb;

    const vec3 L  = normalize(vec3(0.5, 1.0, 0.7));
    float diffuse = max(dot(normalize(vNormal), L), 0.0);
    vec3  ambient = base * 0.25;
    outColor = vec4(ambient + diffuse * base * 0.75, 1.0);
}
