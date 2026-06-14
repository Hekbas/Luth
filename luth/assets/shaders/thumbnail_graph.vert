#version 460

// Graph-aware material preview vertex stage. Delivers the tangent + UV1 the bounded decode and graphed
// normal need (the stock thumbnail_mesh.vert is Position/Normal/UV0 only). The per-draw node-world
// matrix is folded into viewProj on the CPU. Out locations match material_bindings_preview's
// PreviewVaryings, so the generated Slang thumbnail_graph_<hash> fragment pairs with it.

layout(location = 0) in vec3 a_Position;
layout(location = 1) in vec3 a_Normal;
layout(location = 2) in vec2 a_UV0;
layout(location = 3) in vec2 a_UV1;
layout(location = 4) in vec3 a_Tangent;

layout(push_constant) uniform PC { mat4 viewProj; } pc;

layout(location = 0) out vec3 vNormal;
layout(location = 1) out vec3 vTangent;
layout(location = 2) out vec2 vUV0;
layout(location = 3) out vec2 vUV1;

void main()
{
    gl_Position = pc.viewProj * vec4(a_Position, 1.0);
    vNormal  = a_Normal;
    vTangent = a_Tangent;
    vUV0     = a_UV0;
    vUV1     = a_UV1;
}
