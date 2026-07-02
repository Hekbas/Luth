// Debug line fragment: passes the interpolated vertex colour through.
struct FIn { [[vk::location(0)]] float4 color; };

[shader("fragment")]
float4 main(FIn i) : SV_Target
{
    return i.color;
}
