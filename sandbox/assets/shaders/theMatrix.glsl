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

#define CHAR_SIZE ivec2(10, 10)  // Character cell size
#define SPEED 20
#define TRAIL_LENGTH 10
#define FADE_FACTOR 0.5

// Random functions
float rand(float n) { return fract(sin(n) * 43758.5453); }
float rand(vec2 n) { return fract(sin(dot(n, vec2(12.9898,78.233))) * 43758.5453); }

// Matrix character set (ASCII 32-126 + custom)
const uint[64] chars = uint[](
    // Basic ASCII 32-47
    0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u, // 32-35
    0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u, // 36-39
    0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u, // 40-43
    0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u, // 44-47
    
    // Numbers 48-57 (0-9)
    0x3C428181u, 0x4299A581u, 0x7E818181u, 0x42A5817Eu, // 48-51
    0x3C427E7Eu, 0x00426666u, 0x003C4242u, 0x00003C3Cu, // 52-55
    0x18182424u, 0x243C4242u, // 56-57
    
    // Symbols 58-64
    0x42000000u, 0x00000000u, 0x00000000u, 0x00000000u, // 58-61
    0x00000000u, 0x00000000u, 0x3C428181u, // 62-64
    
    // Uppercase Letters 65-90 (A-Z)
    0x4242427Eu, 0x7E407E40u, 0x4070407Eu, // A-C
    0x7E407E40u, 0x40704040u, 0x3C427E4Eu, // D-F
    0x42427E42u, 0x10301008u, 0x0808083Eu, // G-I
    0x464A5262u, 0x4040407Eu, 0x43635549u, // J-L
    0x43434745u, 0x3C424242u, 0x42427E42u, // M-O
    0x424A463Cu, 0x62524242u, 0x3C427E06u, // P-R
    0x3E08083Eu, 0x42422418u, 0x081C2A08u, // S-U
    0x7E201008u, 0x0042427Eu, 0x0810207Eu, // V-X
    0x3E08083Eu, 0x3C42023Cu, // Y-Z
    
    // Symbols 91-95
    0x20407E20u, 0x1E204020u, 0x3E08083Eu, // 91-93
    0x22140814u, 0x007E0202u // 94-95
);

void main()
{
    vec2 uv = v_TexCoord;
    
    // Grid alignment
    ivec2 grid = ivec2(uv * vec2(20.0, 20.0));
    vec2 cellUV = fract(uv * vec2(20.0, 20.0));
    
    // Column animation
    float colTime = u_time * SPEED + grid.x * 10.0;
    float colSeed = rand(grid.x * 0.733);
    float sparkle = step(0.97, rand(vec2(grid.x, floor(colTime))));

    // Character selection
    float charIndex = floor(colTime + grid.y * (0.5 + colSeed));
    uint charCode = chars[int(rand(grid.x + charIndex) * 64.0) % 32 + 16];
    
    // Character pixel calculation
    ivec2 pixel = ivec2(cellUV * vec2(CHAR_SIZE));
    bool lit = (charCode & (1u << (CHAR_SIZE.x - pixel.x - 1))) != 0u;
    
    // Fading trail effect
    float fade = pow(FADE_FACTOR, fract(colTime) * TRAIL_LENGTH);
    float brightness = float(lit) * fade + sparkle * 2.0;
    
    // Color with variations
    vec3 color = mix(vec3(0.1, 0.8, 0.2), 
                  vec3(0.3, 1.0, 0.4), 
                  rand(grid.x + u_time)) * brightness;
    
    // Background dimming
    color *= 0.2 + 0.8 * smoothstep(0.0, 0.3, rand(vec2(grid) + u_time));

    FragColor = vec4(color, 1.0);
}