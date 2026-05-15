cbuffer SkyboxConstants : register(b0)
{
    row_major float4x4 skybox_to_clip;
};

Texture2D skybox_texture : register(t0);
SamplerState skybox_sampler : register(s0);

struct VSInput
{
    float3 position : POSITION;
    float2 texcoord : TEXCOORD0;
};

struct PSInput
{
    float4 position : SV_POSITION;
    float2 texcoord : TEXCOORD0;
};

PSInput VSMain(VSInput input)
{
    PSInput output;
    output.position = mul(float4(input.position, 1.0f), skybox_to_clip);
    output.texcoord = input.texcoord;
    return output;
}

float4 PSMain(PSInput input) : SV_TARGET
{
    const float4 texel = skybox_texture.Sample(skybox_sampler, input.texcoord);
    return float4(texel.rgb, 1.0f);
}
