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

uniform sampler2D u_Source;
uniform bool u_Horizontal;
uniform float u_BlurStrength;

in vec2 v_TexCoord;
out vec4 FragColor;

const float weight[5] = float[] (0.227027, 0.1945946, 0.1216216, 0.054054, 0.016216);

void main()
{
    vec2 tex_offset = 1.0 / textureSize(u_Source, 0) * u_BlurStrength;
    vec3 result = texture(u_Source, v_TexCoord).rgb * weight[0];
    
    if(u_Horizontal)
    {
        for(int i = 1; i < 5; ++i)
        {
            result += texture(u_Source, v_TexCoord + vec2(tex_offset.x * i, 0.0)).rgb * weight[i];
            result += texture(u_Source, v_TexCoord - vec2(tex_offset.x * i, 0.0)).rgb * weight[i];
        }
    }
    else
    {
        for(int i = 1; i < 5; ++i)
        {
            result += texture(u_Source, v_TexCoord + vec2(0.0, tex_offset.y * i)).rgb * weight[i];
            result += texture(u_Source, v_TexCoord - vec2(0.0, tex_offset.y * i)).rgb * weight[i];
        }
    }
    
    FragColor = vec4(result, 1.0);
}