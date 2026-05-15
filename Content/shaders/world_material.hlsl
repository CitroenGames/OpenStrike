#define MATERIAL_FLAG_TRANSLUCENT 1u
#define MATERIAL_FLAG_ALPHA_TEST 2u
#define MATERIAL_FLAG_UNLIT 4u
#define MATERIAL_FLAG_NO_DRAW 8u

cbuffer WorldConstants : register(b0)
{
    row_major float4x4 world_to_clip;
    float4 world_light_direction_ambient;
    float4 world_light_color_intensity;
};

Texture2D world_texture : register(t0);
SamplerState world_sampler : register(s0);

cbuffer MaterialConstants : register(b1)
{
    float4 material_base_color;
    uint material_flags;
    float material_alpha_cutoff;
    float2 material_padding;
};

struct VSInput
{
    float3 position : POSITION;
    float3 normal : NORMAL;
    float2 texcoord : TEXCOORD0;
};

struct PSInput
{
    float4 position : SV_POSITION;
    float3 normal : NORMAL;
    float2 texcoord : TEXCOORD0;
};

PSInput VSMain(VSInput input)
{
    PSInput output;
    output.position = mul(float4(input.position, 1.0f), world_to_clip);
    output.normal = input.normal;
    output.texcoord = input.texcoord;
    return output;
}

float4 PSMain(PSInput input) : SV_TARGET
{
    if ((material_flags & MATERIAL_FLAG_NO_DRAW) != 0u)
    {
        discard;
    }

    const float4 texel = world_texture.Sample(world_sampler, input.texcoord);
    const float alpha = saturate(texel.a * material_base_color.a);
    if ((material_flags & MATERIAL_FLAG_ALPHA_TEST) != 0u && alpha < material_alpha_cutoff)
    {
        discard;
    }

    const float3 base_color = saturate(texel.rgb * material_base_color.rgb);
    if ((material_flags & MATERIAL_FLAG_UNLIT) != 0u)
    {
        return float4(base_color, alpha);
    }

    const float3 normal = normalize(input.normal);
    const float3 light_direction = normalize(world_light_direction_ambient.xyz);
    const float ambient = saturate(world_light_direction_ambient.w);
    const float diffuse = saturate(dot(normal, light_direction)) * max(world_light_color_intensity.w, 0.0f);
    const float3 lit_color = base_color * (ambient + (world_light_color_intensity.rgb * diffuse));
    return float4(saturate(lit_color), alpha);
}
