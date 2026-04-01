// composite_vs.hlsl - pass-through vertex shader for fullscreen quad
struct VSOut {
    float4 pos : SV_POSITION;
    float2 uv  : TEXCOORD0;
};

VSOut main(uint id : SV_VertexID) {
    // Generate a fullscreen triangle from vertex ID (no vertex buffer needed)
    float2 uvs[3] = { float2(0,0), float2(2,0), float2(0,2) };
    float2 pos[3] = { float2(-1,1), float2(3,1), float2(-1,-3) };

    VSOut o;
    o.pos = float4(pos[id], 0, 1);
    o.uv  = uvs[id];
    return o;
}

// ─────────────────────────────────────────────────────────────────────────────

// composite_ps.hlsl - alpha-blend overlay texture over scene
Texture2D    overlayTex : register(t0);
SamplerState linearSamp : register(s0);

float4 main(float4 pos : SV_POSITION, float2 uv : TEXCOORD0) : SV_TARGET {
    float4 overlay = overlayTex.Sample(linearSamp, uv);

    // Premultiplied alpha — standard DXGI overlay blending
    // Source-over: result = src.rgb + dst.rgb * (1 - src.a)
    // We output with full alpha; the swap chain blend state handles the rest.
    return overlay;
}
