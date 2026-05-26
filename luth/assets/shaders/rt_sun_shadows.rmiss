#version 460
#extension GL_EXT_ray_tracing : require

// Miss program paired with rt_sun_shadows.rgen. Raygen initializes payload.visibility = 0.0
// (in shadow) and traces with TERMINATE_ON_FIRST_HIT | OPAQUE — closest-hit + any-hit are skipped
// on a hit, so the payload stays at 0. If the ray reaches tMax without hitting anything, this
// miss fires and sets visibility = 1.0 (fully lit / no occluder along the ray to the sun).

layout(location = 0) rayPayloadInEXT struct { float visibility; } payload;

void main()
{
    payload.visibility = 1.0;
}
