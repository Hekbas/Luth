// Denoiser scaffolding: a no-op copy of the demodulated DI into the denoiser output image. Proves the
// RestirShade -> denoise -> GeometryPass seam (handle re-route + lighting-set rebind). Standalone Set 0
// layout (no global UBO). see arch/rendering-pipeline.md
[[vk::binding(0, 0)]] Sampler2D u_DiIn;
[[vk::binding(1, 0)]] [[vk::image_format("rgba16f")]] RWTexture2D<float4> outDenoised;

[shader("compute")]
[numthreads(8, 8, 1)]
void main(uint3 tid : SV_DispatchThreadID)
{
    int2 pixel = int2(tid.xy);
    uint w, h; outDenoised.GetDimensions(w, h);
    if (pixel.x >= int(w) || pixel.y >= int(h)) return;

    outDenoised[pixel] = u_DiIn.Load(int3(pixel, 0));
}
