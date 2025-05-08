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

uniform sampler2D ssaoInput;
uniform vec2 u_BlurScale;

in vec2 v_TexCoord;
out float FragColor;

void main()
{
    vec2 texelSize = 1.0 / vec2(textureSize(ssaoInput, 0));
    float result = 0.0;
    
    // Bilateral blur with depth awareness
    for(int x = -2; x <= 2; ++x)
    {
        for(int y = -2; y <= 2; ++y)
        {
            vec2 offset = vec2(x, y) * texelSize * u_BlurScale;
            result += texture(ssaoInput, v_TexCoord + offset).r;
        }
    }
    
    FragColor = result / 25.0;
}