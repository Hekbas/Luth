#type vertex
#version 460 core

layout(location = 0) in vec2 a_Position;
out vec2 v_TexCoord;

void main()
{
    gl_Position = vec4(a_Position, 0.0, 1.0);
    v_TexCoord = a_Position * 0.5 + 0.5;
}



#type fragment
#version 460 core

uniform sampler2D gPosition;
uniform sampler2D gNormal;
uniform sampler2D u_Noise;
uniform vec3 u_Samples[64];

uniform mat4 u_Projection;
uniform vec2 u_NoiseScale;
uniform float u_Radius;
uniform float u_Bias;

in vec2 v_TexCoord;
out float FragColor;

void main()
{
    vec3 fragPos = texture(gPosition, v_TexCoord).rgb;
    vec3 normal = normalize(texture(gNormal, v_TexCoord).rgb);
    vec3 randomVec = normalize(texture(u_Noise, v_TexCoord * u_NoiseScale).xyz);
    
    vec3 tangent = normalize(randomVec - normal * dot(randomVec, normal));
    vec3 bitangent = cross(normal, tangent);
    mat3 TBN = mat3(tangent, bitangent, normal);
    
    float occlusion = 0.0;
    for(int i = 0; i < 64; i++)
    {
        vec3 samplePos = TBN * u_Samples[i];
        samplePos = fragPos + samplePos * u_Radius;
        
        vec4 offset = u_Projection * vec4(samplePos, 1.0);
        offset.xyz /= offset.w;
        offset.xy = offset.xy * 0.5 + 0.5;
        
        float sampleDepth = texture(gPosition, offset.xy).z;
        float rangeCheck = smoothstep(0.0, 1.0, u_Radius / abs(fragPos.z - sampleDepth));
        occlusion += (sampleDepth >= samplePos.z + u_Bias ? 1.0 : 0.0) * rangeCheck;
    }
    
    FragColor = 1.0 - (occlusion / 64.0);
}