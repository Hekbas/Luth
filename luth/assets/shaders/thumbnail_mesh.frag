#version 460

// invariant: albedoTex is updated per-bake by ThumbnailPreviewScene via
// vkUpdateDescriptorSets — material bakes point it at the material's albedo
// texture, mesh bakes point it at a 1x1 white default. ImmediateSubmit waits
// before returning, so per-bake update is safe (descriptor never in-flight
// across update).

layout(set = 0, binding = 0) uniform sampler2D albedoTex;

layout(location = 0) in vec3 vNormal;
layout(location = 1) in vec4 vAlbedo;
layout(location = 2) in vec2 vUV;

layout(location = 0) out vec4 outColor;

void main()
{
    vec4 sampled  = texture(albedoTex, vUV);
    vec3 base     = sampled.rgb * vAlbedo.rgb;

    const vec3 L  = normalize(vec3(0.5, 1.0, 0.7));
    float diffuse = max(dot(normalize(vNormal), L), 0.0);
    vec3  ambient = base * 0.25;
    outColor = vec4(ambient + diffuse * base * 0.75, 1.0);
}
