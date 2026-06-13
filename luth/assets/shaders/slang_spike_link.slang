// Slang Phase-0 spike — link-time specialization probe (#156 item 6 / slang#9578). The bigger material
// plan hinges on ONE shared surface-eval body, generic over a shading model, specialized at LINK time and
// emitted into entry points of DIFFERENT stages (raster fragment / RT compute). slang#9578 reports some
// link-spec uses emit Vulkan-incompatible SPIR-V — so this proves it on the pinned version: an IMaterial
// interface + a concrete impl + a generic shadeSurface<M:IMaterial> evaluated from BOTH a [shader("compute")]
// and a [shader("fragment")] entry point. SlangCompiler::CompileModuleEntries composes both into one linked
// program and emits each; the spike then spirv-vals both. GO = both stages emit valid SPIR-V from the one
// specialized generic. see spike #156

interface IMaterial
{
    float3 evalAlbedo(float2 uv);
}

// A concrete bounded shading model (the Filament-style closed set the real series will grow).
struct CheckerMat : IMaterial
{
    float3 evalAlbedo(float2 uv)
    {
        uint cx = uint(uv.x * 8.0);
        uint cy = uint(uv.y * 8.0);
        float c = ((cx + cy) & 1u) == 0u ? 1.0 : 0.2;
        return float3(c, c * 0.5, 1.0 - c);
    }
}

// Generic surface eval — type-checked ONCE, specialized for the concrete M at link time. This is the
// single shared body the bounded-IMaterial seam needs across raster / RT / PT consumers.
float3 shadeSurface<M : IMaterial>(M mat, float2 uv, float3 lightDir, float3 n)
{
    float3 albedo = mat.evalAlbedo(uv);
    float  ndl    = max(dot(n, lightDir), 0.0);
    return albedo * ndl;
}

[[vk::binding(0, 0)]] RWStructuredBuffer<float4> outBuf;

static const float3 kLightDir = float3(0.371391, 0.557086, 0.742781);   // pre-normalized

// Compute entry — the RT-consumer analog (ReSTIR GI / PT evaluate the surface in a compute megakernel).
[shader("compute")]
[numthreads(8, 8, 1)]
void csMain(uint3 tid : SV_DispatchThreadID)
{
    CheckerMat m;
    float2 uv = float2(tid.xy) / 64.0;
    float3 c  = shadeSurface(m, uv, kLightDir, float3(0.0, 0.0, 1.0));
    outBuf[tid.x] = float4(c, 1.0);
}

// Fragment entry — the raster-consumer analog (pbr.frag). Same specialized generic, different stage.
[shader("fragment")]
float4 fsMain(float2 uv : TEXCOORD0) : SV_Target
{
    CheckerMat m;
    float3 c = shadeSurface(m, uv, kLightDir, float3(0.0, 0.0, 1.0));
    return float4(c, 1.0);
}
