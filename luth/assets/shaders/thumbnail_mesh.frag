#version 460

// Thumbnail bake — Lambert + ambient with caller-provided albedo. Mesh bakes
// pass a neutral white so the geometry is the visual signal; material bakes
// pass the material's albedo color so the preview reflects the asset's tint.

layout(location = 0) in vec3 vNormal;
layout(location = 1) in vec4 vAlbedo;

layout(location = 0) out vec4 outColor;

void main()
{
    const vec3 L = normalize(vec3(0.5, 1.0, 0.7));
    float diffuse = max(dot(normalize(vNormal), L), 0.0);
    vec3 ambient  = vAlbedo.rgb * 0.25;
    outColor = vec4(ambient + diffuse * vAlbedo.rgb * 0.75, 1.0);
}
