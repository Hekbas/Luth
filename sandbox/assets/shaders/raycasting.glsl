#type vertex
#version 450 core

layout(location = 0) in vec4 a_Position;
layout(location = 1) in vec2 a_TexCoord;

out vec2 v_TexCoord;

void main()
{
    gl_Position = a_Position;
    v_TexCoord = a_TexCoord;
}



#type fragment
#version 450 core

in vec2 v_TexCoord;
out vec4 FragColor;

uniform vec2 u_resolution;
uniform float u_time;

// Random functions
float rand(float n) { return fract(sin(n) * 43758.5453); }
float rand(vec2 n) { return fract(sin(dot(n, vec2(12.9898,78.233))) * 43758.5453); }

float PlaneIntersect(vec3 ro, vec3 rd, vec3 p, vec3 n)
{
    float denom = dot(rd, n);

    if (abs(denom) > 0.0001)
    {
        float t = dot(p - ro, n) / denom;
        return t >= 0.0 ? t: -1.0;
    }
    return -1.0;
}

void main()
{
    vec2 uv = v_TexCoord - 0.5;
    float t = 1e20;
    vec3 color = vec3(0);

    // Camera 
    vec3 ro = vec3(0.0, 1.0, 3.0);
    vec3 rd = normalize(vec3(uv, -1.0));

    // Light
    vec3 lightDir = vec3(0.5, 0.1, 0.3);

    // Scene
    float tPlane = PlaneIntersect(ro, rd, vec3(0), vec3(0.0, 1.0, 0.0));

    // Hit
    if (tPlane > 0.0 && tPlane < t)
    {
        t = tPlane;
        vec3 pos = ro + rd * t;
        vec3 normal = vec3(0.0, 1.0, 0.0);
        float diff = max(dot(normal, lightDir), 0.0);
        color = vec3(1.0, 0.0, 0.2) * diff;
    }


    FragColor = vec4(color, 1.0);
}
