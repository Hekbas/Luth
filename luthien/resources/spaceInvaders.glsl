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
out vec4 o_Color;

//uniform vec2 u_Resolution;
uniform float u_Time;

#define INVADER_ROWS 2
#define INVADER_COLS 4
#define STAR_COUNT 100

float random(vec2 st) {
    return fract(sin(dot(st.xy, vec2(12.9898,78.233))) * 43758.5453123);
}

bool drawInvader(vec2 uv, vec2 pos, float size, float seed) {
    uv = (uv - pos) / size;
    if(abs(uv.x) > 1.0 || abs(uv.y) > 1.0) return false;
    
    // Invader pattern using bit operations
    // Row patterns stored as hexadecimal
    int[8] pattern = int[8](
        0x3C428181,
        0x4299A581,
        0x7E818181,
        0x42A5817E,
        0x3C427E7E,
        0x00426666,
        0x003C4242,
        0x00003C3C
    );

    ivec2 grid = ivec2((uv * 0.5 + 0.5) * 8.0);
    if(grid.x < 0 || grid.x >= 8 || grid.y < 0 || grid.y >= 8) return false;
    
    return (pattern[grid.y] & (1 << grid.x)) != 0;
}

void main()
{
    vec2 uv = (v_TexCoord - 0.5);// * vec2(u_Resolution.x/u_Resolution.y, 1.0);
    vec3 col = vec3(0.0);
    
    // Starfield background
    for(int i = 0; i < STAR_COUNT; i++) {
        vec2 starPos = vec2(random(vec2(i)), random(vec2(i*2)));
        if(length(uv - (starPos - 0.5)*5.0) < 0.001 * random(vec2(i*3))) {
            col = vec3(1.0);
        }
    }
    
    // Draw invaders
    float invaderSize = 0.03;
    float spacing = 0.14;
    float wave = sin(u_Time * 2.0) * 0.2;
    
    for(int y = 0; y < INVADER_ROWS; y++) {
        for(int x = 0; x < INVADER_COLS; x++) {
            vec2 pos = vec2(
                (x - INVADER_COLS/2) * spacing + wave,
                (y - INVADER_ROWS/2) * spacing * 0.5 + 0.2
            );
            if(drawInvader(uv, pos, invaderSize, float(x + y*INVADER_COLS))) {
                col = mix(vec3(0.4, 0.8, 0.3), vec3(0.8, 0.3, 0.4), 
                       sin(u_Time * 5.0 + float(x+y)*10.0) * 0.5 + 0.5);
            }
        }
    }
    
    // Draw player
    vec2 playerPos = vec2(0.0, -0.4);
    vec2 playerSize = vec2(0.15, 0.05);
    if(abs(uv.x - playerPos.x) < playerSize.x && 
       abs(uv.y - playerPos.y) < playerSize.y) {
        col = vec3(1.0);
    }
    
    // Draw bullets
    vec2 bulletPos = vec2(0.0, -0.3 + fract(u_Time * 2.0) * 0.5);
    if(length(uv - bulletPos) < 0.01) {
        col = vec3(1.0, 0.0, 0.0);
    }
    
    o_Color = vec4(col, 1.0);
}