#type vertex
#version 450 core

layout(location = 0) in vec2 a_Position;

out vec2 fragCoord;

void main()
{
    fragCoord = a_Position;
    gl_Position = vec4(a_Position, 0.0, 1.0);
}


#type fragment
#version 450 core

in vec2 fragCoord;
out vec4 fragColor;

uniform float u_Time;

void main()
{
    vec2 uv = fragCoord;
    vec3 col = 0.5 + 0.5 * cos(u_Time + uv.xyx + vec3(0,2,4));
    fragColor = vec4(col, 1.0);
}
