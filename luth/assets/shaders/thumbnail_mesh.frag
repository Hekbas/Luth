#version 460

// Thumbnail bake — neutral Lambert + ambient. Fixed lighting and white albedo
// so every mesh renders with consistent visual signal — the geometry, not its
// in-engine material context. Matches the architecture-spec direction for
// mesh thumbnails (preview-scene neutral).

layout(location = 0) in vec3 vNormalWS;
layout(location = 0) out vec4 outColor;

void main()
{
    const vec3 L      = normalize(vec3(0.5, 1.0, 0.7));
    const vec3 albedo = vec3(0.85);
    float diffuse = max(dot(normalize(vNormalWS), L), 0.0);
    vec3 ambient  = albedo * 0.25;
    outColor = vec4(ambient + diffuse * albedo * 0.75, 1.0);
}
