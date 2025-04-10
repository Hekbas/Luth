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

layout(location = 0) out vec3 v_Normal;
layout(location = 1) out vec2 v_TexCoord0;
layout(location = 2) out vec2 v_TexCoord1;
layout(location = 3) out vec3 v_Tangent;
layout(location = 4) out vec3 v_Bitangent;
layout(location = 5) out vec3 v_WorldPos;

void main()
{
    v_WorldPos = vec3(ubo.model * vec4(a_Position, 1.0));
    gl_Position = ubo.proj * ubo.view * vec4(v_WorldPos, 1.0);
    
    mat3 normalMatrix = transpose(inverse(mat3(ubo.model)));
    v_Normal = normalize(normalMatrix * a_Normal);
    v_Tangent = normalize(normalMatrix * a_Tangent);
    
    // Calculate bitangent using cross product with handedness
    v_Bitangent = cross(v_Normal, v_Tangent);
    v_TexCoord0 = a_TexCoord0;
    v_TexCoord1 = a_TexCoord1;
}



#type geometry
#version 450

layout(triangles) in;
layout(line_strip, max_vertices = 6) out;

layout(binding = 0) uniform UniformBufferObject {
    mat4 model;
    mat4 view;
    mat4 proj;
} ubo;

uniform float u_NormalLength = 0.5;
uniform float u_ViewDistanceScale = 0.01;

in vec3 v_Normal[];
in vec3 v_WorldPos[];

void main() {
    for(int i = 0; i < 3; i++) {
        // Get world space components
        vec3 position = v_WorldPos[i];
        vec3 normal = v_Normal[i];
        
        // Calculate true normal direction and length compensation
        float normalScale = length(normal);
        vec3 normalDir = normal / normalScale;
        
        // Calculate perspective-aware length
        vec4 clipPos = ubo.proj * ubo.view * vec4(position, 1.0);
        float depthScale = abs(clipPos.w);  // Use perspective divide component
        float adjustedLength = u_NormalLength * depthScale * u_ViewDistanceScale;
        
        // Calculate final end position
        vec3 endPosition = position + normalDir * (adjustedLength / normalScale);
        
        // Transform to clip space
        vec4 clipStart = ubo.proj * ubo.view * vec4(position, 1.0);
        vec4 clipEnd = ubo.proj * ubo.view * vec4(endPosition, 1.0);
        
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
