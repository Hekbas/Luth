// ReSTIR GI shared types + helpers (Ouyang 2021) — twin of restir_gi_common.glsl. GIReservoir layout
// mirrors the C++ GPUGIReservoir; distinct from the DI Reservoir. see arch/rendering-pipeline.md
import restir_common;   // Luminance + light structs

public static const float GI_PI = 3.14159265358979;

// 64 B, std430 — a 1-bounce path sample: secondary hit x_s, its normal n_s (oct), outgoing radiance
// L_o, reservoir state, and the receiver's self-carried world pos + normal for temporal validation.
public struct GIReservoir
{
    public float3 samplePos;       public float W;
    public float3 sampleRadiance;  public float wSum;
    public float2 sampleNormalOct; public uint  M;  public uint age;
    public float3 visPos;          public uint  visNormalPacked;
}

public void GIReservoirReset(out GIReservoir r)
{
    r.samplePos = float3(0.0); r.W = 0.0;
    r.sampleRadiance = float3(0.0); r.wSum = 0.0;
    r.sampleNormalOct = float2(0.0); r.M = 0u; r.age = 0u;
    r.visPos = float3(0.0); r.visNormalPacked = 0u;
}

// Octahedral encode — inverse of restir_common OctDecode (Cigolle et al. [0,1] mapping).
public float2 OctEncode(float3 n)
{
    n /= (abs(n.x) + abs(n.y) + abs(n.z));
    float2 e = (n.z >= 0.0)
             ? n.xy
             : (float2(1.0) - abs(n.yx)) * float2(n.x >= 0.0 ? 1.0 : -1.0, n.y >= 0.0 ? 1.0 : -1.0);
    return e * 0.5 + 0.5;
}

// Stream one path-sample candidate (weighted reservoir sampling, Chao 1982). invariant: M>0 ⇒ samplePos
// valid — the first candidate seeds even at w=0 (an unlit hit is a confident zero, not an empty reservoir).
public void GIReservoirUpdate(inout GIReservoir r, float3 xs, float3 Lo, float2 nsOct, float w, float rnd)
{
    r.wSum += w;
    r.M    += 1u;
    if (r.M == 1u || (r.wSum > 0.0 && rnd * r.wSum <= w))
    {
        r.samplePos = xs; r.sampleRadiance = Lo; r.sampleNormalOct = nsOct;
    }
}

// Merge a source reservoir's sample (reconnection Jacobian pre-folded into targetPdfAtCurr by the caller).
public void GIReservoirMerge(inout GIReservoir c, float3 xs, float3 Lo, float2 nsOct, uint ageSrc,
                             float targetPdfAtCurr, float Wsrc, uint Msrc, float rnd)
{
    float w = targetPdfAtCurr * Wsrc * float(Msrc);
    c.wSum += w;
    c.M    += Msrc;
    if (c.M == Msrc || (c.wSum > 0.0 && rnd * c.wSum <= w))
    {
        c.samplePos = xs; c.sampleRadiance = Lo; c.sampleNormalOct = nsOct; c.age = ageSrc;
    }
}

public void GIReservoirFinalize(inout GIReservoir r, float targetPdfAtSurface)
{
    r.W = (targetPdfAtSurface > 0.0 && r.M > 0u) ? (r.wSum / (float(r.M) * targetPdfAtSurface)) : 0.0;
}

// GI target — albedo-free (matches the demodulated output). 1/dist^2 + cos at x_s ride in the Jacobian.
public float GiTargetFunction(float3 xs, float3 Lo, float3 receiverPos, float3 receiverN)
{
    float3 L = xs - receiverPos;
    float  d2 = dot(L, L);
    if (d2 <= 0.0) return 0.0;
    L *= rsqrt(d2);
    return Luminance(Lo) * max(dot(receiverN, L), 0.0);
}

// Pixar branchless orthonormal basis (Duff et al.) — TBN from a unit normal.
public void BranchlessONB(float3 n, out float3 b1, out float3 b2)
{
    float s = (n.z >= 0.0) ? 1.0 : -1.0;
    float a = -1.0 / (s + n.z);
    float b = n.x * n.y * a;
    b1 = float3(1.0 + s * n.x * n.x * a, s * b, -s * n.x);
    b2 = float3(b, s + n.y * n.y * a, -n.y);
}

// Cosine-weighted hemisphere direction around n; out pdf = cos/pi (the initial reservoir source pdf).
public float3 SampleCosHemisphere(float3 n, float2 u, out float pdf)
{
    float3 t, bt; BranchlessONB(n, t, bt);
    float r   = sqrt(u.y);
    float phi = 2.0 * GI_PI * u.x;
    float z   = sqrt(max(0.0, 1.0 - u.y));   // = cos(theta)
    pdf = z / GI_PI;
    float3 local = float3(r * cos(phi), r * sin(phi), z);
    return normalize(t * local.x + bt * local.y + n * local.z);
}

// Reconnection-shift Jacobian (Ouyang 2021 Eq. 11): crossed ratios, both cosines at the sample point.
public float GiReconnectionJacobian(float3 receiverNew, float3 receiverOrig, float3 samplePos, float3 sampleNormal)
{
    float3 vN = receiverNew  - samplePos;
    float3 vO = receiverOrig - samplePos;
    float  dN = dot(vN, vN);
    float  dO = dot(vO, vO);
    if (dN <= 0.0 || dO <= 0.0) return 0.0;
    float cosN = clamp(dot(sampleNormal, vN * rsqrt(dN)), 0.0, 1.0);
    float cosO = clamp(dot(sampleNormal, vO * rsqrt(dO)), 0.0, 1.0);
    float j = (cosN * dO) / (cosO * dN);
    return (isinf(j) || isnan(j)) ? 0.0 : j;
}

// Two-stage guard (RTXDI): reject incompatible reuse (J outside [1/10,10]); else clamp to [1/3,3].
public bool GiValidateJacobian(inout float j)
{
    if (j > 10.0 || j < 0.1) return false;
    j = clamp(j, 1.0 / 3.0, 3.0);
    return true;
}
