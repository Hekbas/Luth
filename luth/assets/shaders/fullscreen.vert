#version 450

layout(location = 0) out vec2 v_TexCoord;

void main()
{
    // Fullscreen triangle from gl_VertexIndex (0, 1, 2)
    // No vertex buffer needed — just vkCmdDraw(cmd, 3, 1, 0, 0)
    v_TexCoord = vec2((gl_VertexIndex << 1) & 2, gl_VertexIndex & 2);
    gl_Position = vec4(v_TexCoord * 2.0 - 1.0, 0.0, 1.0);
}
