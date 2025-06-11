#type vertex
#version 460 core

layout(location = 0) in vec3 a_Position;
layout(location = 1) in vec2 a_TexCoord;

layout(location = 0) out vec2 v_UV;

void main()
{
    v_UV = a_TexCoord;
    gl_Position = vec4(a_Position, 1.0);
}



#type fragment
#version 460 core

in vec2 v_UV;

layout(location = 0) out vec4 FragColor;

layout(binding = 0) uniform sampler2D u_Scene;
layout(binding = 1) uniform sampler2D u_Bloom;

uniform float u_Time;
uniform float u_BloomStrength;
uniform float u_GrainAmount;
uniform float u_Sharpness;
uniform float u_AberrationOffset;
uniform float u_VignetteAmount;
uniform float u_VignetteHardness;
uniform int   u_ToneMapOperator;
uniform float u_Exposure;
uniform float u_Contrast;
uniform float u_Saturation;
uniform vec3  u_ShadowBalance;
uniform vec3  u_MidtoneBalance;
uniform vec3  u_HighlightBalance;

float rand(vec2 co) {
    return fract(sin(dot(co.xy ,vec2(12.9898,78.233))) * 43758.5453);
}

vec3 ApplyGrain(vec2 uv, vec3 col) {
    float g = (rand(uv * u_Time) - 0.5) * u_GrainAmount;
    return col + g;
}

vec3 ApplyChromatic(vec2 uv, vec3 color) {
    // Apply different offsets per channel
    float rOffset = u_AberrationOffset * 1.0;
    float gOffset = u_AberrationOffset * 0.0;
    float bOffset = u_AberrationOffset * -1.0;
    
    // Sample with offset per channel
    vec2 rUV = clamp(uv + vec2(rOffset, 0.0), 0.0, 1.0);
    vec2 gUV = clamp(uv + vec2(gOffset, 0.0), 0.0, 1.0);
    vec2 bUV = clamp(uv + vec2(bOffset, 0.0), 0.0, 1.0);
    
    // Use the combined color but with shifted sampling
    return vec3(
        texture(u_Scene, rUV).r * color.r,
        texture(u_Scene, gUV).g * color.g,
        texture(u_Scene, bUV).b * color.b
    );
}

vec3 ApplyVignette(vec2 uv, vec3 col, float amount, float hardness) {
    vec2 centeredUV = uv - 0.5;
    float dist = length(centeredUV);
    float radius = mix(0.7071, 0.0, amount);
    float smoothness = mix(0.5, 0.01, hardness);
    float vignette = smoothstep(radius, radius + smoothness, dist);
    return col * (1.0 - vignette);
}

// Tone mapping operators ==================================================
vec3 ToneMapLinear(vec3 color, float exposure) {
    return color * exposure;
}

vec3 ToneMapReinhard(vec3 color, float exposure) {
    color *= exposure;
    return color / (1.0 + color);
}

vec3 ToneMapReinhardMod(vec3 color, float exposure) {
    const float L_white = 4.0;
    color *= exposure;
    return color * (1.0 + color / (L_white * L_white)) / (1.0 + color);
}

vec3 ToneMapACES(vec3 color, float exposure) {
    color *= exposure;
    
    const float a = 2.51;
    const float b = 0.03;
    const float c = 2.43;
    const float d = 0.59;
    const float e = 0.14;
    
    return clamp((color * (a * color + b)) / (color * (c * color + d) + e), 0.0, 1.0);
}

vec3 ToneMapFilmic(vec3 color, float exposure) {
    color *= exposure;
    
    vec3 x = max(vec3(0.0), color - 0.004);
    return (x * (6.2 * x + 0.5)) / (x * (6.2 * x + 1.7) + 0.06);
}

vec3 ToneMapUncharted2(vec3 color, float exposure) {
    color *= exposure;
    
    const float A = 0.15;
    const float B = 0.50;
    const float C = 0.10;
    const float D = 0.20;
    const float E = 0.02;
    const float F = 0.30;
    
    return ((color * (A * color + C * B) + D * E) / (color * (A * color + B) + D * F)) - E / F;
}

// Color adjustments
vec3 ApplyContrast(vec3 color, float contrast) {
    return pow(color, vec3(1.0 / contrast));
}

vec3 ApplySaturation(vec3 color, float saturation) {
    const vec3 luminance = vec3(0.2126, 0.7152, 0.0722);
    float lum = dot(color, luminance);
    return mix(vec3(lum), color, saturation);
}

vec3 ApplyToneMapping(vec3 color) {
    // Apply selected tone mapping operator
    switch(u_ToneMapOperator) {
        case 0:  color = ToneMapLinear(color, u_Exposure); break;
        case 1:  color = ToneMapReinhard(color, u_Exposure); break;
        case 2:  color = ToneMapReinhardMod(color, u_Exposure); break;
        case 3:  color = ToneMapACES(color, u_Exposure); break;
        case 4:  color = ToneMapFilmic(color, u_Exposure); break;
        case 5:  color = ToneMapUncharted2(color, u_Exposure); break;
        default: color = ToneMapACES(color, u_Exposure); break;
    }
    
    // Post-tone mapping adjustments
    color = ApplyContrast(color, u_Contrast);
    color = ApplySaturation(color, u_Saturation);
    
    return color;
}

vec3 ApplyColorBalance(vec3 color) {
    color = mix(color, color * u_ShadowBalance,    0.33);
    color = mix(color, color * u_MidtoneBalance,   0.33);
    color = mix(color, color * u_HighlightBalance, 0.33);
    return color;
}

void main()
{
    // [1] Sample base scene and bloom
    vec3 col = texture(u_Scene, v_UV).rgb;
    vec3 bloom = texture(u_Bloom, v_UV).rgb;

    // [2] Add bloom BEFORE tone mapping
    col += bloom * u_BloomStrength;

    // [3] Tone mapping (HDR -> LDR)
    col = ApplyToneMapping(col);

    // [4] Color balance (works best in LDR space)
    col = ApplyColorBalance(col);

    // [5] Sharpening - BEFORE stylistic effects
    vec2 texelSize = 1.0 / textureSize(u_Scene, 0);
    vec3 blurred = (
        texture(u_Scene, v_UV + vec2(texelSize.x, 0)).rgb +
        texture(u_Scene, v_UV - vec2(texelSize.x, 0)).rgb +
        texture(u_Scene, v_UV + vec2(0, texelSize.y)).rgb +
        texture(u_Scene, v_UV - vec2(0, texelSize.y)).rgb
    ) * 0.25;
    col = col + (col - blurred) * u_Sharpness;

    // [6] Chromatic aberration
    col = ApplyChromatic(v_UV, col);

    // [7] Grain
    col = ApplyGrain(v_UV, col);

    // [8] Vignette - LAST effect
    col = ApplyVignette(v_UV, col, u_VignetteAmount, u_VignetteHardness);

    // [9] Gamma Correction
    const float gamma = 2.2;
    col = pow(col, vec3(1.0 / gamma));

    FragColor = vec4(col, 1.0);
}