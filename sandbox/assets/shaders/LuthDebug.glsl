#type vertex
#version 450

layout(binding = 0) uniform UniformBufferObject {
    mat4 model;
    mat4 view;
    mat4 proj;
} ubo;

layout(location = 0) in vec3 a_Position;
layout(location = 1) in vec3 a_Normal;
layout(location = 2) in vec2 a_TexCoord0;
layout(location = 3) in vec2 a_TexCoord1;
layout(location = 4) in vec3 a_Tangent;

layout(location = 0) out vec3 v_ViewSpaceNormal;
layout(location = 1) out vec3 v_ViewSpacePosition;

void main()
{
    // Calculate view-space position
    vec4 viewPos = ubo.view * ubo.model * vec4(a_Position, 1.0);
    v_ViewSpacePosition = viewPos.xyz;
    
    // Calculate normal matrix (view-space)
    mat3 normalMatrix = transpose(inverse(mat3(ubo.view * ubo.model)));
    v_ViewSpaceNormal = normalize(normalMatrix * a_Normal);
    
    gl_Position = ubo.proj * viewPos;
}



#type geometry
#version 450

layout(binding = 0) uniform UniformBufferObject {
    mat4 model;
    mat4 view;
    mat4 proj;
} ubo;

layout(triangles) in;
layout(line_strip, max_vertices = 6) out;

uniform float u_NormalLength = 0.5;

layout(location = 0) in vec3 v_ViewSpaceNormal[];
layout(location = 1) in vec3 v_ViewSpacePosition[];

void main() {
    for(int i = 0; i < 3; i++) {
        // Get view-space components
        vec3 position = v_ViewSpacePosition[i];
        vec3 normal = normalize(v_ViewSpaceNormal[i]);
        
        // Calculate end position in view space
        vec3 endPosition = position + normal * u_NormalLength;
        
        // Convert to clip space
        vec4 clipStart = ubo.proj * vec4(position, 1.0);
        vec4 clipEnd = ubo.proj * vec4(endPosition, 1.0);
        
        // Emit vertices
        gl_Position = clipStart;
        EmitVertex();
        gl_Position = clipEnd;
        EmitVertex();
        EndPrimitive();
    }
}



#type fragment
#version 450

layout(location = 0) out vec4 FragColor;

uniform vec3 u_NormalColor = vec3(0.0, 1.0, 0.0); // Default green

void main() {
    FragColor = vec4(u_NormalColor, 1.0);
}
