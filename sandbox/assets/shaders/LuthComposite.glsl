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

uniform sampler2D u_Scene;
uniform sampler2D u_SSAO;
uniform sampler2D u_Bloom;
uniform float u_Exposure;
uniform float u_BloomStrength;
uniform float u_SSAOStrength;

in vec2 v_TexCoord;
out vec4 FragColor;

vec3 ACESFilm(vec3 x)
{
    float a = 2.51;
    float b = 0.03;
    float c = 2.43;
    float d = 0.59;
    float e = 0.14;
    return clamp((x*(a*x + b))/(x*(c*x + d) + e), 0.0, 1.0);
}

void main()
{
    vec3 sceneColor = texture(u_Scene, v_TexCoord).rgb;
    float ssao = texture(u_SSAO, v_TexCoord).r;
    vec3 bloom = texture(u_Bloom, v_TexCoord).rgb;
    
    // Apply SSAO to ambient
    sceneColor *= mix(1.0, ssao, u_SSAOStrength);
    
    // Add bloom
    sceneColor += bloom * u_BloomStrength;
    
    // Tone mapping
    sceneColor = ACESFilm(sceneColor * u_Exposure);
    
    // Gamma correction
    sceneColor = pow(sceneColor, vec3(1.0/2.2));
    
    FragColor = vec4(sceneColor, 1.0);
}