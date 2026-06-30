// GTAO main integration — faithful port of XeGTAO::MainPass (Intel, MIT) minus bent-normal + multi-mip.
// Half-res; reconstructs view-space normals from depth, integrates horizon-based AO, writes R8 visibility.
// Reference: github.com/GameTechDev/XeGTAO; Jimenez et al. 2016. Standalone Set 0.
[[vk::binding(0, 0)]] Sampler2D u_LinearDepth;
[[vk::binding(1, 0)]] [[vk::image_format("r8")]] RWTexture2D<float> u_RawAO;

struct GTAOUBO
{
    float intensity, radius, falloff, power;
    int   sliceCount, stepsPerSlice, enabled, visualize;
    float2 invResolution, invFullResolution;
};
[[vk::binding(2, 0)]] ConstantBuffer<GTAOUBO> g;

struct PC
{
    float2 projParams;   // (P[0][0], |P[1][1]|)
    float  nearZ, farZ;
    uint   frameIndex, _pad0, _pad1, _pad2;
};
[[vk::push_constant]] PC pc;

static const float PI      = 3.141592653589793;
static const float PI_HALF = 1.570796326794896;

float3 ComputeViewspacePosition(float2 uv, float viewZ)
{
    float2 ndc = uv * 2.0 - 1.0;
    return float3(ndc / pc.projParams * viewZ, viewZ);
}

float FastACos(float x)
{
    float ax = abs(x);
    float r  = -0.156583 * ax + PI_HALF;
    r *= sqrt(max(1.0 - ax, 0.0));
    return (x >= 0.0) ? r : PI - r;
}

float2 SpatioTemporalNoise(int2 coord)
{
    uint   idx = pc.frameIndex & 63u;
    float2 rot = float2(float(idx) * 47.0, float(idx) * 17.0);
    float n0 = frac(52.9829189 * frac(dot(float2(coord) + rot,       float2(0.06711056, 0.00583715))));
    float n1 = frac(52.9829189 * frac(dot(float2(coord) + rot * 1.7, float2(0.06711056, 0.00583715))));
    return float2(n0, n1);
}

float3 ReconstructNormal(float2 uv, float3 posC, float viewZ)
{
    float2 ir = g.invResolution;
    float2 uvL = uv - float2(ir.x, 0.0);
    float2 uvR = uv + float2(ir.x, 0.0);
    float2 uvT = uv - float2(0.0, ir.y);
    float2 uvB = uv + float2(0.0, ir.y);

    float zL = u_LinearDepth.Sample(uvL).r;
    float zR = u_LinearDepth.Sample(uvR).r;
    float zT = u_LinearDepth.Sample(uvT).r;
    float zB = u_LinearDepth.Sample(uvB).r;

    float3 pL = ComputeViewspacePosition(uvL, zL);
    float3 pR = ComputeViewspacePosition(uvR, zR);
    float3 pT = ComputeViewspacePosition(uvT, zT);
    float3 pB = ComputeViewspacePosition(uvB, zB);

    float3 dpdx = (abs(zR - viewZ) < abs(zL - viewZ)) ? (pR - posC) : (posC - pL);
    float3 dpdy = (abs(zB - viewZ) < abs(zT - viewZ)) ? (pB - posC) : (posC - pT);
    return normalize(cross(dpdy, dpdx));
}

[shader("compute")]
[numthreads(8, 8, 1)]
void main(uint3 tid : SV_DispatchThreadID)
{
    int2 coord = int2(tid.xy);
    int2 size  = int2(float2(1.0) / g.invResolution + float2(0.5));
    if (coord.x >= size.x || coord.y >= size.y) return;

    float2 uv    = (float2(coord) + 0.5) * g.invResolution;
    float  viewZ = u_LinearDepth.Sample(uv).r;

    if (viewZ >= pc.farZ * 0.999) { u_RawAO[coord] = 1.0; return; }

    viewZ *= 0.99999;   // nudge toward camera to hide depth-imprecision self-intersection

    float3 pixCenter = ComputeViewspacePosition(uv, viewZ);
    float3 viewVec   = normalize(-pixCenter);
    float3 normal    = ReconstructNormal(uv, pixCenter, viewZ);

    float effectRadius = g.radius;
    float falloffRange = g.falloff * effectRadius;
    float falloffFrom  = effectRadius * (1.0 - g.falloff);
    float falloffMul   = -1.0 / max(falloffRange, 1e-5);
    float falloffAdd   = falloffFrom / max(falloffRange, 1e-5) + 1.0;

    float2 pixelViewSize = viewZ * (2.0 / pc.projParams) * g.invFullResolution;
    float screenspaceRadius = effectRadius / max(pixelViewSize.x, 1e-5);

    float visibility = clamp((10.0 - screenspaceRadius) / 100.0, 0.0, 1.0) * 0.5;

    const float pixelTooCloseThreshold = 1.3;
    float minS = pixelTooCloseThreshold / max(screenspaceRadius, 1e-3);

    float2 noise       = SpatioTemporalNoise(coord);
    float  noiseSlice  = noise.x;
    float  noiseSample = noise.y;
    const float sampleDistributionPower = 2.0;

    for (int s = 0; s < g.sliceCount; ++s)
    {
        float sliceK = (float(s) + noiseSlice) / float(g.sliceCount);
        float phi    = sliceK * PI;
        float cosPhi = cos(phi);
        float sinPhi = sin(phi);

        float2 omega = float2(cosPhi, -sinPhi) * screenspaceRadius;

        float3 directionVec      = float3(cosPhi, -sinPhi, 0.0);
        float3 orthoDirectionVec = directionVec - dot(directionVec, viewVec) * viewVec;
        float3 axisVec           = normalize(cross(orthoDirectionVec, viewVec));
        float3 projectedNormalVec = normal - axisVec * dot(normal, axisVec);
        float projNLen = length(projectedNormalVec);
        float signNorm = sign(dot(orthoDirectionVec, projectedNormalVec));
        float cosNorm  = clamp(dot(projectedNormalVec, viewVec) / max(projNLen, 1e-5), 0.0, 1.0);
        float n        = signNorm * FastACos(cosNorm);

        float lowHorizonCos0 = cos(n + PI_HALF);
        float lowHorizonCos1 = cos(n - PI_HALF);
        float horizonCos0    = lowHorizonCos0;
        float horizonCos1    = lowHorizonCos1;

        for (int step = 0; step < g.stepsPerSlice; ++step)
        {
            float stepBaseNoise = float(s + step * g.stepsPerSlice) * 0.6180339887498948482;
            float stepNoise     = frac(noiseSample + stepBaseNoise);

            float sRel = (float(step) + stepNoise) / float(g.stepsPerSlice);
            sRel  = pow(sRel, sampleDistributionPower);
            sRel += minS;

            float2 sampleOffPixels = round(sRel * omega);
            float2 sampleOff       = sampleOffPixels * g.invFullResolution;

            float2 uv0 = uv + sampleOff;
            float2 uv1 = uv - sampleOff;

            bool  valid0 = uv0.x >= 0.0 && uv0.x <= 1.0 && uv0.y >= 0.0 && uv0.y <= 1.0;
            bool  valid1 = uv1.x >= 0.0 && uv1.x <= 1.0 && uv1.y >= 0.0 && uv1.y <= 1.0;
            float z0     = valid0 ? u_LinearDepth.Sample(uv0).r : pc.farZ;
            float z1     = valid1 ? u_LinearDepth.Sample(uv1).r : pc.farZ;

            float3 sPos0   = ComputeViewspacePosition(uv0, z0);
            float3 sPos1   = ComputeViewspacePosition(uv1, z1);
            float3 sDelta0 = sPos0 - pixCenter;
            float3 sDelta1 = sPos1 - pixCenter;
            float  sDist0  = length(sDelta0);
            float  sDist1  = length(sDelta1);

            float3 sDir0 = sDelta0 / max(sDist0, 1e-6);
            float3 sDir1 = sDelta1 / max(sDist1, 1e-6);

            float w0 = clamp(sDist0 * falloffMul + falloffAdd, 0.0, 1.0);
            float w1 = clamp(sDist1 * falloffMul + falloffAdd, 0.0, 1.0);
            if (!valid0) w0 = 0.0;
            if (!valid1) w1 = 0.0;

            float shc0 = dot(sDir0, viewVec);
            float shc1 = dot(sDir1, viewVec);

            shc0 = lerp(lowHorizonCos0, shc0, w0);
            shc1 = lerp(lowHorizonCos1, shc1, w1);

            horizonCos0 = max(horizonCos0, shc0);
            horizonCos1 = max(horizonCos1, shc1);
        }

        projNLen = lerp(projNLen, 1.0, 0.05);   // XeGTAO high-slope fudge

        float h0 = -FastACos(horizonCos1);
        float h1 = +FastACos(horizonCos0);

        float sinN  = sin(n);
        float iarc0 = (cosNorm + 2.0 * h0 * sinN - cos(2.0 * h0 - n)) * 0.25;
        float iarc1 = (cosNorm + 2.0 * h1 * sinN - cos(2.0 * h1 - n)) * 0.25;
        visibility += projNLen * (iarc0 + iarc1);
    }

    visibility /= float(g.sliceCount);
    visibility  = pow(clamp(visibility, 0.0, 1.0), g.power);
    visibility  = max(0.03, visibility);   // XeGTAO: disallow total occlusion

    u_RawAO[coord] = visibility;
}
