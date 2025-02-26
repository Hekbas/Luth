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

//uniform vec2 u_Resolution;
uniform float u_Time;

void main()
{
    vec2 uv = fragCoord; //(fragCoord - 0.5 * u_Resolution.xy) / u_Resolution.y;
    float zoom = exp(0.2 * u_Time) - 1.0;
    
    // Map uv to Mandelbrot plane
    vec2 c = uv / zoom + vec2(-0.743643887037151, 0.131825904205330); //cool spot
    vec2 z = vec2(0.0);
    int iter;
    int maxIter = 400;
    int currIter = 1 + min(int(min(exp(0.4 * u_Time * 1.5), 4.0 * u_Time)), maxIter);
    
    // Mandelbrot
    for(iter = 0; iter < currIter; iter++) {
        if(dot(z, z) > 4.0) break;
        z = vec2(z.x*z.x - z.y*z.y, 2.0*z.x*z.y) + c;
    }
    
    // Grayscale + contrast
    float col = float(iter) / float(currIter);
    col = pow(col, 1.2);
    fragColor = vec4(vec3(col), 1.0);
}
