// Fullscreen triangle from SV_VertexID (0,1,2); no vertex buffer (vkCmdDraw(cmd, 3, 1, 0, 0)).
struct VOut
{
    float4 clipPos : SV_Position;
    [[vk::location(0)]] float2 texCoord;
};

[shader("vertex")]
VOut main(uint vid : SV_VertexID)
{
    VOut o;
    o.texCoord = float2((vid << 1) & 2, vid & 2);
    o.clipPos  = float4(o.texCoord * 2.0 - 1.0, 0.0, 1.0);
    return o;
}
