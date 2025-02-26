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

struct HitInfo
{
    float t;
    vec3 position;
    vec3 normal;
    vec3 color;
    bool hit;
};

float PlaneIntersect(vec3 ro, vec3 rd, vec3 p, vec3 n)
{
    float denom = dot(rd, n);
    if (abs(denom) > 1e-6) {
        float t = dot(p - ro, n) / denom;
        return t >= 0.0 ? t : -1.0;
    }
    return -1.0;
}

float SphereIntersect(vec3 ro, vec3 rd, vec3 center, float radius)
{
    vec3 oc = ro - center;
    float a = dot(rd, rd);
    float b = 2.0 * dot(oc, rd);
    float c = dot(oc, oc) - radius*radius;
    float d = b*b - 4.0*a*c;
    if (d < 0.0) return -1.0;
    float t = (-b - sqrt(d)) / (2.0*a);
    return t > 0.0 ? t : (-b + sqrt(d)) / (2.0*a);
}

HitInfo SceneIntersect(vec3 ro, vec3 rd)
{
    HitInfo hit;
    hit.t = 1e20;
    hit.hit = false;

    // Check floor plane
    float tPlane = PlaneIntersect(ro, rd, vec3(0, -0.5, 0), vec3(0, 1, 0));
    if (tPlane > 0.0 && tPlane < hit.t) {
        hit.t = tPlane;
        hit.position = ro + rd * hit.t;
        hit.normal = vec3(0, 1, 0);
        hit.color = vec3(0.9, 0.3, 0.2);
        hit.hit = true;
    }

    // Check sphere
    float tSphere = SphereIntersect(ro, rd, vec3(0, 0, 0), 1.0);
    if (tSphere > 0.0 && tSphere < hit.t) {
        hit.t = tSphere;
        hit.position = ro + rd * hit.t;
        hit.normal = normalize(hit.position - vec3(0, 0, 0));
        hit.color = vec3(0.4, 0.7, 0.9);
        hit.hit = true;
    }

    return hit;
}

vec3 CalculateLighting(vec3 pos, vec3 normal, vec3 color)
{
    vec3 lightPos = vec3(2, 5, 3);
    vec3 lightDir = lightPos - pos;
    float lightDist = length(lightDir);
    lightDir /= lightDist;
    
    return color;
}

vec3 TraceRay(vec3 ro, vec3 rd)
{
    vec3 throughput = vec3(1.0);
    vec3 color = vec3(0.0);
    
    for(int bounce = 0; bounce < 2; bounce++) {
        HitInfo hit = SceneIntersect(ro, rd);
        if (!hit.hit) {
            color += throughput * vec3(0.5, 0.7, 1.0);
            break;
        }
        
        // Direct lighting
        color += throughput * CalculateLighting(hit.position, hit.normal, hit.color);
    }
    return color;
}

void main()
{
    vec2 uv = (v_TexCoord - 0.5) * vec2(u_resolution.x / u_resolution.y, 1.0);
    
    vec3 ro = vec3(0, 1, 5);
    vec3 rd = normalize(vec3(uv, -1.0));
    
    vec3 color = TraceRay(ro, rd);
    FragColor = vec4(pow(color, vec3(1.0 / 2.2)), 1.0); // Gamma correction
}
