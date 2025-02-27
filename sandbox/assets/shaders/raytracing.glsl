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
uniform int u_displayMode;
uniform int u_SSAA;
uniform int u_maxBounces;
uniform float u_softShadowFactor;
uniform int u_applyTonemapping;
uniform int u_applyGamma;
uniform float u_exposure;
uniform float u_gamma;

#define MAX_LIGHTS 4
#define PI 3.141592653589793
#define EPSILON 0.001

struct Material {
    vec3 albedo;
    float roughness;
    float metallic;
    vec3 emission;
};

struct Light {
    vec3 position;
    vec3 color;
    float intensity;
};

struct HitInfo {
    float t;
    vec3 position;
    vec3 normal;
    Material mat;
    bool hit;
};

struct DisplayComponents {
    vec3 finalColor;
    vec3 rayDir;
    vec3 albedo;
    vec3 specular;
    vec3 radiance;
    vec3 normal;
    vec3 worldPos;
};

// Scene uniforms
uniform Material u_floorMaterial;
uniform Material u_sphereMaterials[3];
uniform vec3 u_spherePositions[3];
uniform Light u_lights[MAX_LIGHTS];
uniform int u_numLights;


vec3 pcg3d(vec3 v) {
    v = v * 114007.0 + vec3(1.0);
    v = fract(v * vec3(0.1031, 0.1030, 0.0973));
    v += dot(v, v.yzx + 19.19);
    return fract((v.xxy + v.yzz) * v.zyx);
}

DisplayComponents InitDisplayComponents() {
    return DisplayComponents(
        vec3(0.0),
        vec3(0.0),
        vec3(0.0),
        vec3(0.0),
        vec3(0.0),
        vec3(0.0),
        vec3(0.0)
    );
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

float PlaneIntersect(vec3 ro, vec3 rd, vec3 p, vec3 n)
{
    float denom = dot(rd, n);
    return denom < 0.0 ? dot(p - ro, n) / denom : -1.0;
}

HitInfo SceneIntersect(vec3 ro, vec3 rd)
{
    HitInfo hit;
    hit.t = 1e20;
    hit.hit = false;

    // Floor
    float tPlane = PlaneIntersect(ro, rd, vec3(0.0, -0.5, 0.0), vec3(0.0, 1.0, 0.0));
    if (tPlane > 0.0 && tPlane < hit.t) {
        hit.t = tPlane;
        hit.position = ro + rd * hit.t;
        hit.normal = vec3(0, 1, 0);
        hit.mat.albedo = u_floorMaterial.albedo;
        hit.mat.roughness = u_floorMaterial.roughness;
        hit.mat.metallic = u_floorMaterial.metallic;
        hit.mat.emission = vec3(0);
        hit.hit = true;
    }

    // Spheres
    for(int i = 0; i < 3; i++) {
        float tSphere = SphereIntersect(ro, rd, u_spherePositions[i], 1.0);
        if (tSphere > 0.0 && tSphere < hit.t) {
            hit.t = tSphere;
            hit.position = ro + rd * hit.t;
            hit.normal = normalize(hit.position - u_spherePositions[i]);
            hit.mat.albedo = u_sphereMaterials[i].albedo;
            hit.mat.roughness = u_sphereMaterials[i].roughness;
            hit.mat.metallic = u_sphereMaterials[i].metallic;
            hit.mat.emission = vec3(0);
            hit.hit = true;
        }
    }
    return hit;
}

vec3 FresnelSchlick(float cosTheta, vec3 F0) {
    return F0 + (1.0 - F0) * pow(1.0 - cosTheta, 5.0);
}

float DistributionGGX(vec3 N, vec3 H, float roughness) {
    float a = roughness*roughness;
    float a2 = a*a;
    float NdotH = max(dot(N, H), 0.0);
    float denom = (NdotH * NdotH * (a2 - 1.0) + 1.0);
    return a2 / (PI * denom * denom);
}

float GeometrySchlickGGX(float NdotV, float roughness) {
    float r = (roughness + 1.0);
    float k = (r*r) / 8.0;
    return NdotV / (NdotV * (1.0 - k) + k);
}

float SoftShadow(vec3 pos, vec3 lightPos, float hardness)
{
    vec3 lightDir = lightPos - pos;
    float lightDistance = length(lightDir);
    vec3 dir = normalize(lightDir);
    
    float shadow = 0.0;
    int samples = 16;
    float radius = mix(0.1, 0.01, hardness);
    
    for(int i = 0; i < samples; i++) {
        vec3 random = pcg3d(vec3(gl_FragCoord.xy, i));
        vec3 jitter = random * radius;
        vec3 samplePos = pos + jitter;
        
        HitInfo shHit = SceneIntersect(samplePos + dir * EPSILON, dir);
        if(!shHit.hit || shHit.t > lightDistance) {
            shadow += 1.0;
        }
    }
    return shadow / float(samples);
}

void CalculateLighting(inout DisplayComponents dc, HitInfo hit, vec3 viewDir)
{
    if(!hit.hit) return;

    vec3 N = hit.normal;
    vec3 V = viewDir;
    vec3 albedo = hit.mat.albedo;
    vec3 F0 = mix(vec3(0.04), albedo, hit.mat.metallic);

    for(int i = 0; i < MAX_LIGHTS; i++) {
        if(i >= u_numLights) break;
        
        vec3 L = normalize(u_lights[i].position - hit.position);
        vec3 H = normalize(V + L);
        float distance = length(u_lights[i].position - hit.position);
        float attenuation = 1.0 / (distance * distance);
        float shadow = SoftShadow(hit.position, u_lights[i].position, u_softShadowFactor);
        
        vec3 radiance = u_lights[i].color * u_lights[i].intensity * shadow * attenuation;
        float NdotL = max(dot(N, L), 0.0);

        // Fixed Fresnel
        vec3 F = FresnelSchlick(max(dot(H, V), 0.0), F0);
        
        // Fixed Geometry
        float NdotV = max(dot(N, V), 0.0);
        float G = GeometrySchlickGGX(NdotV, hit.mat.roughness);

        // Diffuse
        vec3 kD = (vec3(1.0) - F) * (1.0 - hit.mat.metallic);
        vec3 diffuse = kD * albedo * radiance * NdotL / PI;

        // Specular
        float NDF = DistributionGGX(N, H, hit.mat.roughness);
        vec3 specular = (NDF * G * F) / (4.0 * NdotV * NdotL + 0.001) * radiance * NdotL;

        dc.radiance += diffuse;
        dc.specular += specular;
    }
}

void GetSurfaceData(inout DisplayComponents dc, HitInfo hit, vec3 rd)
{
    dc.rayDir = rd;
    if(hit.hit) {
        dc.albedo = hit.mat.albedo;
        dc.normal = hit.normal;
        dc.worldPos = hit.position;
    } else {
        dc.rayDir = (rd + 1.0) / 2.0;
    }
}

DisplayComponents TraceRay(vec3 ro, vec3 rd)
{
    DisplayComponents dc = InitDisplayComponents();
    vec3 throughput = vec3(1.0);
    
    for(int bounce = 0; bounce < u_maxBounces; bounce++) {
        HitInfo hit = SceneIntersect(ro, rd);

        if(bounce == 0) {
            GetSurfaceData(dc, hit, rd);
        }

        if(hit.hit) CalculateLighting(dc, hit, -rd);
        else {
            dc.finalColor += throughput * vec3(0.5, 0.7, 1.0) * (bounce == 0 ? 1.0 : 0.2);
            break;
        }
        
        dc.finalColor += throughput * (dc.radiance + dc.specular);
        ro = hit.position + hit.normal * EPSILON;
        rd = reflect(rd, hit.normal);
        throughput *= 0.2;
    }
    return dc;
}

vec3 GetVisualizationColor(DisplayComponents dc)
{
    switch(u_displayMode) {
        case 1: return dc.rayDir;
        case 2: return dc.albedo;
        case 3: return dc.specular;
        case 4: return dc.radiance;
        case 5: return (dc.normal + 1.0) / 2.0; //suave :3
        //case 5: return dc.normal;
        case 6: return dc.worldPos;
        default: return dc.finalColor;
    }
}

vec3 TraceAndVisualize(vec2 uv)
{
    vec3 ro = vec3(0.0, 0.5, 5.0);
    vec3 rd = normalize(vec3(uv, -1.0));
    DisplayComponents dc = TraceRay(ro, rd);
    return GetVisualizationColor(dc);
}

vec3 SampleWithSSAA()
{
    vec3 color = vec3(0);
    const int AA = u_SSAA;
    
    for(int y = 0; y < AA; y++) {
        for(int x = 0; x < AA; x++) {
            vec2 offset = vec2(x, y) / float(AA) - 0.5;
            vec2 uv = (v_TexCoord - 0.5 + offset/u_resolution) * 
                     vec2(u_resolution.x/u_resolution.y, 1.0);
            color += TraceAndVisualize(uv);
        }
    }
    return color / float(AA*AA);
}

vec3 PostProcessing(vec3 color)
{
    if(u_displayMode != 0) return color;
    if(u_applyTonemapping == 1) {
        color *= u_exposure;
        color = color / (color + vec3(1.0));
    }
    if(u_applyGamma == 1) {
        color = pow(color, vec3(1.0/u_gamma));
    }
    return color;
}

void main()
{
    vec3 color;
    
    if(u_SSAA > 1) {
        color = SampleWithSSAA();
    } else {
        vec2 uv = (v_TexCoord - 0.5) * vec2(u_resolution.x/u_resolution.y, 1.0);
        color = TraceAndVisualize(uv);
    }
    
    FragColor = vec4(PostProcessing(color), 1.0);
}
