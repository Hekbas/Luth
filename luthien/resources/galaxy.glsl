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

//uniform vec2 u_resolution;
uniform float u_time;

#define PI 3.14159265359
#define ARM_COUNT 2
#define STAR_DENSITY 0.8
#define GALAXY_SIZE 0.8

// Noise functions for procedural generation
float hash(float n) { return fract(sin(n) * 43758.5453); }
float noise(vec2 p) { return fract(sin(dot(p, vec2(127.1, 311.7)))) * 43758.5453; }

float fbm(vec2 uv) {
    float sum = 0.0;
    float amp = 0.5;
    for(int i = 0; i < 5; i++) {
        sum += amp * noise(uv);
        uv *= 2.0;
        amp *= 0.5;
    }
    return sum;
}

void main()
{
    float aspect = 16.0 / 9.0;
    vec2 uv = v_TexCoord.xy - 0.5;
    uv.x *= aspect;
    float dist = length(uv);
    
    // Base galaxy
    float galaxyMask = smoothstep(GALAXY_SIZE, 0.0, dist);
    
    // Spiral arms using polar coordinates
    vec2 polar = vec2(length(uv), atan(uv.y, uv.x));
    float angle = polar.y + 3.0 * log(polar.x + 1.0) + u_time * 0.1;
    
    // Spiral pattern
    float arms = 0.0;
    for(int i = 0; i < ARM_COUNT; i++) {
        float a = angle + (PI * 2.0 * float(i)) / float(ARM_COUNT);
        arms += 0.5 * sin(a * 5.0 + polar.x * 10.0) + 0.5;
    }
    arms = smoothstep(0.3, 0.7, arms/float(ARM_COUNT));
    
    // Nebula colors
    vec3 color = mix(
        vec3(0.3, 0.1, 0.5),    // Purple
        vec3(0.1, 0.1, 0.5),    // Blue
        fbm(uv * 2.0)
    );
    
    // Core
    color = mix(color, vec3(1.0, 0.8, 0.4), smoothstep(0.1, 0.05, dist));
    
    // Star field
    vec2 starUV = uv * 500.0;
    vec2 ipos = floor(starUV);
    vec2 fpos = fract(starUV);
    float star = pow(hash(noise(ipos)), 50.0) * 
                step(0.995, hash(ipos.x + ipos.y * 100.0));
    
    // Combine elements
    color = mix(color, vec3(1.0), star * galaxyMask);
    color *= arms * galaxyMask;
    
    // Rotation
    color *= 0.8 + 0.2 * sin(polar.x * 20.0 - u_time * 2.0);
    
    // bloom?
    color += smoothstep(0.7, 1.0, color) * 0.3;

    // Gamma correction
    color = pow(color, vec3(0.4545));
    
    FragColor = vec4(color, 1.0);
}