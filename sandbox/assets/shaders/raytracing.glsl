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

struct Camera {
    vec3 origin;
    vec3 direction;
    vec3 lookAt;
    float fov;
    bool useLookAt;
};

struct AmbientLight {
    vec3 skyColor;
    vec3 groundColor;
    float intensity;
};

struct PointLight {
    vec3 position;
    vec3 color;
    float intensity;
};

struct Fog {
    bool enabled;
    vec3 color;
    float density;
    float start;
    float end;
};

struct Material {
    vec3 albedo;
    vec3 emissive;
    float roughness;
    float metallic;
    float ior;
    float transparency;
};

struct HitInfo {
    float t;
    vec3 position;
    vec3 normal;
    Material mat;
    bool hit;
};

struct ShadingData {
    vec3 rayDir;
    vec3 albedo;
    vec3 worldPos;
    vec3 normal;
    vec3 fresnel;
    vec3 radiance;
    vec3 diffuse;
    vec3 specular;
    vec3 emissive;
    float depth;
    vec3 finalColor;
};

ShadingData sd;
ShadingData GetDefaultShadingData() {
    return ShadingData(
        vec3(0.0),  // rayDir
        vec3(0.0),  // albedo
        vec3(0.0),  // worldPos
        vec3(0.0),  // normal
        vec3(0.0),  // fresnel
        vec3(0.0),  // radiance
        vec3(0.0),  // diffuse
        vec3(0.0),  // specular
        vec3(0.0),  // emissive
        0.0,        // depth
        vec3(0.0)   // finalColor
    );
}

// Scene uniforms
uniform Camera u_camera;
uniform AmbientLight u_ambientLight;
uniform PointLight u_pointLights[MAX_LIGHTS];
uniform int u_numPointLights;
uniform Fog u_fog;
uniform Material u_floorMaterial;
uniform vec3 u_spherePositions[3];
uniform Material u_sphereMaterials[3];

// Random
float pcg1d(float v) {
    return fract(sin(v * 114007.0) * 43758.5453);
}
vec2 pcg2d(vec2 v) {
    return fract(
        sin(v.xyx * vec3(114007.0, 114007.0, 1.0)) * 
        vec3(43758.5453, 43758.5453, 43758.5453)
    ).xy;
}
vec3 pcg3d(vec3 v) {
    v = v * 114007.0 + vec3(1.0);
    v = fract(v * vec3(0.1031, 0.1030, 0.0973));
    v += dot(v, v.yzx + 19.19);
    return fract((v.xxy + v.yzz) * v.zyx);
}
float hash(float v) {
    return fract(sin(v) * 43758.5453);
}
float hash(vec2 v) {
    return fract(sin(dot(v, vec2(12.9898, 78.233))) * 43758.5453);
}
float getSeed(int v) {
    vec2 fragCoord = gl_FragCoord.xy * (float(v + 1) * 0.618);
    return hash(fragCoord + u_time);
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

        // Checkerboard
        vec2 uv = hit.position.xz * 1.0;
        ivec2 tile = ivec2(floor(uv));
        bool isEven = (tile.x + tile.y) % 2 == 0;
        hit.mat.albedo = u_floorMaterial.albedo * (isEven ? vec3(0) : vec3(1));

        hit.mat.emissive = u_floorMaterial.emissive;
        hit.mat.roughness = u_floorMaterial.roughness;
        hit.mat.metallic = u_floorMaterial.metallic;
        hit.mat.ior = u_floorMaterial.ior;
        hit.mat.transparency = u_floorMaterial.transparency;
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
            hit.mat.emissive = u_sphereMaterials[i].emissive;
            hit.mat.roughness = u_sphereMaterials[i].roughness;
            hit.mat.metallic = u_sphereMaterials[i].metallic;
            hit.mat.ior = u_sphereMaterials[i].ior;
            hit.mat.transparency = u_sphereMaterials[i].transparency;
            hit.hit = true;
        }
    }
    return hit;
}

// Fresnel reflectance using Schlick's approximation
// Part of Cook-Torrance BRDF for specular reflection
// F = F0 + (1 - F0) * (1 - cosθ)^5
// - F0: Base reflectivity at normal incidence
// - cosθ: Dot product between view and half vectors
// Info: https://odederell3d.blog/2018/09/18/fresnel-reflections/
vec3 FresnelSchlick(float cosTheta, vec3 F0) {
    return F0 + (vec3(1.0) - F0) * pow(1.0 - cosTheta, 5.0);
}

// Normal Distribution Function (NDF) using Trowbridge-Reitz GGX
// Models microsurface orientation for specular highlights
// Part of Cook-Torrance BRDF
// D(n,h,a) = a^2 / [π((n·h)^2(a^2-1)+1)^2]
float DistributionGGX(vec3 N, vec3 H, float roughness) {
    float a = roughness*roughness;
    float a2 = a*a;
    float NdotH = max(dot(N, H), 0.0);
    float denom = (NdotH * NdotH * (a2 - 1.0) + 1.0);
    return a2 / (PI * denom * denom);
}

// Geometry Shadowing using Schlick-GGX approximation
// Models shadowing/masking of microsurface details
// k = (a + 1)^2 / 8 (UE4 remapping for direct lighting)
// G(n,v,a) = n·v / (n·v(1-k) + k)
float GeometrySchlickGGX(float NdotV, float roughness) {
    float r = (roughness + 1.0);
    float k = (r*r) / 8.0;
    return NdotV / (NdotV * (1.0 - k) + k);
}

// Smith's method for geometry shadowing
// Combines view and light direction shadowing
// G(n,v,l,a) = G1(n,v,a) * G1(n,l,a)
float GeometrySmith(vec3 N, vec3 V, vec3 L, float roughness) {
    float NdotV = max(dot(N, V), 0.0);
    float NdotL = max(dot(N, L), 0.0);
    float ggx1 = GeometrySchlickGGX(NdotV, roughness);
    float ggx2 = GeometrySchlickGGX(NdotL, roughness);
    return ggx1 * ggx2;
}

// Too noisy :(
float SoftShadow(vec3 pos, vec3 normal, vec3 lightPos, float hardness) {
    vec3 lightDir = lightPos - pos;
    float lightDistance = length(lightDir);
    vec3 dir = normalize(lightDir);
    
    float shadow = 0.0;
    int samples = 16;
    float radius = mix(0.1, 0.01, hardness);
    
    for(int i = 0; i < samples; i++) {
        vec3 random = pcg3d(vec3(gl_FragCoord.xy, i));
        vec3 jitter = random * radius;
        vec3 samplePos = pos + normal * EPSILON + jitter;
        
        HitInfo shHit = SceneIntersect(samplePos + dir * EPSILON, dir);
        if(!shHit.hit || shHit.t > lightDistance) {
            shadow += 1.0;
        }
    }
    return shadow / float(samples);
}

// Warning! Expensive, not recomended to set samples over 16.
// I should really find another way to do soft shadows...
// Computes penumbra soft shadows by jittering ray origins across an area light,
// using stratified sampling and adaptive penumbra scaling to reduce noise.
// Traces multiple rays toward the light, blending visibility based on blocker distance.
float SoftShadowSARS(vec3 pos, vec3 normal, vec3 lightPos, float hardness) {
    vec3 toLight = lightPos - pos;
    float lightDist = length(toLight);
    vec3 lightDir = toLight / lightDist;
    
    // Adaptive radius: Penumbra scales with distance
    float penumbraRadius = mix(0.1, 0.01, hardness) * lightDist;
    
    float shadow = 0.0;
    int samples = 16;
    
    // Stratified grid + rotation to reduce banding
    float angle = 30.0 * (pcg1d(gl_FragCoord.x + gl_FragCoord.y * 1920.0));
    mat2 rot = mat2(cos(angle), -sin(angle), sin(angle), cos(angle));
    
    for(int i = 0; i < samples; i++) {
        // Stratified grid sample
        vec2 grid = vec2(i % 4, i / 4) / 4.0; // 4x4 grid
        vec2 jitter = (grid + 0.5 + pcg2d(vec2(i)) * 0.25); // Jitter within cell
        
        // Map to disk with concentric mapping (better than random)
        vec2 offset = 2.0 * jitter - 1.0;
        if (abs(offset.x) > abs(offset.y)) 
            offset = vec2(offset.x > 0.0 ? 1.0 : -1.0, offset.y / abs(offset.x));
        else 
            offset = vec2(offset.x / abs(offset.y), offset.y > 0.0 ? 1.0 : -1.0);
        
        offset = rot * offset; // Apply rotation
        vec3 samplePos = lightPos + penumbraRadius
                        * (offset.x * cross(lightDir, normal)
                        + offset.y * cross(lightDir, cross(lightDir, normal)));
        
        // Trace from surface to light sample
        vec3 shadowRayDir = normalize(samplePos - pos);
        HitInfo shHit = SceneIntersect(pos + normal * EPSILON, shadowRayDir);
        
        // Smooth visibility based on blocker distance
        if(!shHit.hit) {
            shadow += 1.0;
        } else {
            float t = shHit.t / lightDist;
            shadow += smoothstep(0.0, 1.0, t * t); // Quadratic falloff
        }
    }
    return shadow / float(samples);
}

vec3 CalculateEmissiveGlow(vec3 ro, vec3 rd) {
    vec3 glow = vec3(0.0);
    const int glowSamples = 4;
    const float glowRadius = 2.8;
    const float glowStrength = 64.0;
    const float glowFalloff = 8.5;
    
    for(int i = 0; i < 3; i++) {
        if(length(u_sphereMaterials[i].emissive) > 0.0) {
            // Sphere data
            vec3 center = u_spherePositions[i];
            float radius = 1.0;
            vec3 emissive = u_sphereMaterials[i].emissive;
            
            // Calculate closest point on ray to sphere center
            vec3 oc = ro - center;
            float tca = dot(oc, rd);
            float d2 = dot(oc, oc) - tca * tca;
            float radius2 = radius * radius;
            
            // If ray passes near sphere
            if(d2 < radius2 * glowRadius) {
                // Estimate glow
                float dist = sqrt(d2);
                float glowIntensity = glowStrength * 
                    pow(1.0 - smoothstep(0.0, radius * glowRadius, dist), glowFalloff);
                
                // Add jittered samples for softer look
                for(int s = 0; s < glowSamples; s++) {
                    vec3 jitter = normalize(pcg3d(vec3(s, glowIntensity, u_time)));
                    glow += emissive * glowIntensity * 0.1 * 
                        pow(1.0 - dist/(radius * glowRadius), 2.0);
                }
            }
        }
    }
    return glow / float(glowSamples);
}

// Full PBR lighting calculation using Cook-Torrance BRDF
// https://graphicscompendium.com/gamedev/15-pbr
// Lo = (kD * albedo/π + (D*G*F)/(4(n·v)(n·l))) * L_i * (n·l)
// Where:
// - kD = (1 - F)(1 - metallic) - Diffuse reflection coefficient
// - D = Normal Distribution Function (GGX)
// - G = Geometric Attenuation Function (Smith)
// - F = Fresnel Reflectance (Schlick)
// Energy conserved through metallic workflow:
// - Dielectrics (non-metals): Both diffuse and specular
// - Metals: Specular only (kD=0 when metallic=1)
void CalculateLighting(inout vec3 throughput, HitInfo hit, vec3 viewDir)
{
    if(!hit.hit) return;

    vec3 N = normalize(hit.normal);
    vec3 V = normalize(viewDir);
    vec3 albedo = hit.mat.albedo;
    float roughness = hit.mat.roughness;
    vec3 F0 = mix(vec3(0.04), hit.mat.albedo, hit.mat.metallic);

    // Point Lights
    for(int i = 0; i < MAX_LIGHTS; i++) {
        if(i >= u_numPointLights) break;
        
        vec3 L = normalize(u_pointLights[i].position - hit.position);
        vec3 H = normalize(V + L);
        float distance = length(u_pointLights[i].position - hit.position);
        float attenuation = 1.0 / (distance * distance);
        float shadow = SoftShadowSARS(hit.position, hit.normal, u_pointLights[i].position, u_softShadowFactor);
        
        float NdotL = max(dot(N, L), 0.0);
        float NdotV = max(dot(N, V), 0.0);
        float HdotV = max(dot(H, V), 0.0);

        vec3 radiance = u_pointLights[i].color * u_pointLights[i].intensity * shadow * attenuation * NdotL;

        // Fresnel
        vec3 F = FresnelSchlick(max(dot(L, H), 0.0), F0);
        
        // Diffuse (Lambert)
        vec3 kD = (vec3(1.0) - F) * (1.0 - hit.mat.metallic);
        vec3 diffuse = kD * albedo * radiance / PI;

        // Specular (Cook-Torrance)
        float D = DistributionGGX(N, H, roughness);
        float G = GeometrySmith(N, V, L, roughness);
        vec3 specular = (D * G * F) / (4.0 * NdotV * NdotL + EPSILON) * radiance;

        sd.fresnel += F * throughput;
        sd.radiance += radiance * throughput;
        sd.diffuse += diffuse * throughput;
        sd.specular += specular * throughput;
        throughput *= F;
    }

    throughput = vec3(1);   // Reset Throughput

    // Emissive
    for(int i = 0; i < u_spherePositions.length(); i++) {
        //if(u_sphereMaterials[i].emissive == vec3(0.0)) continue;

        vec3 L = normalize(u_spherePositions[i] - hit.position);
        vec3 H = normalize(V + L);
        float distance = length(u_spherePositions[i] - hit.position);
        float attenuation = 1.0 / (distance * distance);
        float shadow = SoftShadowSARS(hit.position, hit.normal, u_spherePositions[i], u_softShadowFactor);
        
        float NdotL = max(dot(N, L), 0.0);
        float NdotV = max(dot(N, V), 0.0);
        float HdotV = max(dot(H, V), 0.0);

        vec3 radiance = u_sphereMaterials[i].emissive * 400.0 * shadow * attenuation * NdotL;

        // Fresnel
        vec3 F = FresnelSchlick(max(dot(L, H), 0.0), F0);
        
        // Diffuse (Lambert)
        vec3 kD = (vec3(1.0) - F) * (1.0 - hit.mat.metallic);
        vec3 diffuse = kD * albedo * radiance / PI;

        // Specular (Cook-Torrance)
        float D = DistributionGGX(N, H, roughness);
        float G = GeometrySmith(N, V, L, roughness);
        vec3 specular = (D * G * F) / (4.0 * NdotV * NdotL + EPSILON) * radiance;

        sd.fresnel += F * throughput;
        sd.radiance += radiance * throughput;
        sd.diffuse += diffuse * throughput;
        sd.specular += specular * throughput;
        throughput *= F;
    }
}

void GetSurfaceData(HitInfo hit, vec3 rd) {
    if(hit.hit) {
        sd.rayDir = rd;
        sd.albedo = hit.mat.albedo;
        sd.normal = hit.normal;
        sd.worldPos = hit.position;
    } else {
        sd.rayDir = (rd + 1.0) / 2.0;
    }
}

void CalculateDepth(HitInfo hit) {
    if(hit.hit) {
        float distance = length(sd.worldPos - u_camera.origin);
        float distRatio = 4.0 * distance / u_fog.end;
        sd.depth = 1 - exp(-distRatio*u_fog.density * distRatio*u_fog.density);
    } else {
        sd.depth = 1.0;
    }    
}

vec3 GetAmbientColor(vec3 rayDir) {
    float horizonMix = smoothstep(-0.3, 0.3, rayDir.y);
    vec3 skyColor = mix(
        u_ambientLight.groundColor * u_ambientLight.intensity,
        u_ambientLight.skyColor * u_ambientLight.intensity,
        horizonMix
    );
    
    return skyColor;
}

void TraceRay(vec3 ro, vec3 rd)
{
    sd = GetDefaultShadingData();
    vec3 throughput = vec3(1.0);
    vec3 emissiveGlow = vec3(0.0);

    for(int bounce = 0; bounce <= u_maxBounces; bounce++) {        
        HitInfo hit = SceneIntersect(ro, rd);

        if(bounce == 0) {
            GetSurfaceData(hit, rd); // For visualization modes
            CalculateDepth(hit);
            emissiveGlow += CalculateEmissiveGlow(ro, rd) * throughput;
        }
        
        if(hit.hit) {           
            sd.emissive += hit.mat.emissive * throughput;
            CalculateLighting(throughput, hit, -rd);
        }
        else {
            vec3 ambient = GetAmbientColor(rd);
            sd.finalColor += throughput * ambient;
            break;
        }
        

        if (hit.mat.transparency < 0.5) {
            ro = hit.position + hit.normal * EPSILON;
            rd = reflect(rd, hit.normal);
        }
        else {
            // Check for sphere back hit
            bool isBack = dot(rd, hit.normal) > 0.0 ? true : false;
            vec3 N = isBack ? -hit.normal : hit.normal;
            float eta = isBack ? hit.mat.ior : 1/hit.mat.ior;
            ro = hit.position - N * EPSILON;
            rd = refract(normalize(rd), normalize(N), eta);
        }
    }
    sd.finalColor += sd.diffuse + sd.specular + sd.emissive + emissiveGlow;

    if (u_fog.enabled) {
        sd.finalColor = mix(sd.finalColor, u_fog.color, sd.depth);
    }
}

vec3 GetVisualizationColor()
{
    switch(u_displayMode) {
        case 1: return sd.rayDir;
        case 2: return sd.albedo;
        case 3: return sd.worldPos;
        case 4: return (sd.normal + 1.0) / 2.0; //suave :3
        case 5: return sd.fresnel;
        case 6: return sd.radiance;
        case 7: return sd.diffuse;
        case 8: return sd.specular;
        case 9: return sd.emissive;
        case 10: return vec3(sd.depth);
        default: return sd.finalColor; // 0
    }
}

vec3 TraceAndVisualize(mat3 camBasis, vec2 uv)
{
    vec3 ro = u_camera.origin;
    vec3 rd = normalize(camBasis * vec3(uv, -1.0));  
    TraceRay(ro, rd);
    return GetVisualizationColor();
}

vec3 SampleWithSSAA(mat3 camBasis, float focalScale)
{
    vec3 color = vec3(0);
    const int AA = u_SSAA;
    
    for(int y = 0; y < AA; y++) {
        for(int x = 0; x < AA; x++) {
            vec2 offset = vec2(x, y) / float(AA) - 0.5;
            vec2 uv = (v_TexCoord - 0.5 + offset/u_resolution) * 
                     vec2(u_resolution.x/u_resolution.y, 1.0) * focalScale;
            color += TraceAndVisualize(camBasis, uv);
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

mat3 GetCameraBasis(vec3 forward) {
    vec3 worldUp = vec3(0.0, 1.0, 0.0);
    vec3 right = normalize(cross(forward, worldUp));
    vec3 up = normalize(cross(right, forward));
    return mat3(right, up, forward);
}

void main()
{
    vec3 color;
    float aspectRatio = u_resolution.x / u_resolution.y;

    // Camera
    vec3 camForward = u_camera.useLookAt == true ?
        normalize(u_camera.lookAt - u_camera.origin) : normalize(u_camera.direction);
    mat3 camBasis = GetCameraBasis(camForward); 
    float focalScale = tan(radians(u_camera.fov) * 0.5);
    
    if(u_SSAA > 1) {
        color = SampleWithSSAA(camBasis, focalScale);
    } else {
        vec2 uv = (v_TexCoord - 0.5) * vec2(aspectRatio, 1.0) * focalScale;
        color = TraceAndVisualize(camBasis, uv);
    }
    
    FragColor = vec4(PostProcessing(color), 1.0);
}
