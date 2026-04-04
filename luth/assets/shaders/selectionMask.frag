#version 450

// Selection mask fragment — writes solid 1.0 to the red channel.
// Used to mark pixels belonging to selected entities.
layout(location = 0) out vec4 outMask;

void main()
{
    outMask = vec4(1.0, 0.0, 0.0, 1.0);
}
