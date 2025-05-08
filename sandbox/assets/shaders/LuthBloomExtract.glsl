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
uniform float u_Threshold;

in vec2 v_TexCoord;
out vec4 FragColor;

void main()
{
    vec3 color = texture(u_Source, v_TexCoord).rgb;
    float brightness = dot(color, vec3(0.2126, 0.7152, 0.0722));
    FragColor = vec4(color * smoothstep(u_Threshold, u_Threshold + 0.1, brightness), 1.0);
}