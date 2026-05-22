#version 450

// Slim G-buffer material-ID preview decoder. Reads R16U integer texture and
// emits a distinct RGB palette color per material slot. Uses the same prime-
// fraction hash as pbr.frag's EntityID debug visualization for consistency.

layout(location = 0) in vec2 v_TexCoord;
layout(location = 0) out vec4 outColor;

layout(set = 0, binding = 0) uniform usampler2D u_SourceMatID;

void main()
{
    uint id = texture(u_SourceMatID, v_TexCoord).r;

    if (id == 0u)
    {
        // Slot 0 (cleared pixels / unmapped material) — pure black.
        outColor = vec4(0.0, 0.0, 0.0, 1.0);
        return;
    }

    // Cheap prime-fraction hash — matches pbr.frag EntityID debug viz palette
    // shape so the user can visually correlate material IDs across debug modes.
    float f = float(id);
    vec3 c = vec3(fract(f * 0.123), fract(f * 0.456), fract(f * 0.789));
    outColor = vec4(c, 1.0);
}
