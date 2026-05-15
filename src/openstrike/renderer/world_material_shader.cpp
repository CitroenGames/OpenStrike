#include "openstrike/renderer/world_material_shader.hpp"

namespace openstrike
{
const char* world_material_vertex_shader_source()
{
    return R"(
cbuffer WorldConstants : register(b0)
{
    row_major float4x4 world_to_clip;
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

PSInput main(VSInput input)
{
    PSInput output;
    output.position = mul(float4(input.position, 1.0f), world_to_clip);
    output.normal = input.normal;
    output.texcoord = input.texcoord;
    return output;
}
)";
}

const char* world_material_pixel_shader_source()
{
    return R"(
#define MATERIAL_FLAG_TRANSLUCENT 1u
#define MATERIAL_FLAG_ALPHA_TEST 2u
#define MATERIAL_FLAG_UNLIT 4u
#define MATERIAL_FLAG_NO_DRAW 8u

Texture2D world_texture : register(t0);
SamplerState world_sampler : register(s0);

cbuffer MaterialConstants : register(b1)
{
    float4 material_base_color;
    uint material_flags;
    float material_alpha_cutoff;
    float2 material_padding;
};

struct PSInput
{
    float4 position : SV_POSITION;
    float3 normal : NORMAL;
    float2 texcoord : TEXCOORD0;
};

float4 main(PSInput input) : SV_TARGET
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
    const float light = saturate(0.42f + (abs(normal.z) * 0.42f) + (abs(normal.x) * 0.10f) + (abs(normal.y) * 0.06f));
    return float4(saturate(base_color * light), alpha);
}
)";
}

const char* skybox_vertex_shader_source()
{
    return R"(
cbuffer SkyboxConstants : register(b0)
{
    row_major float4x4 skybox_to_clip;
};

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

PSInput main(VSInput input)
{
    PSInput output;
    output.position = mul(float4(input.position, 1.0f), skybox_to_clip);
    output.texcoord = input.texcoord;
    return output;
}
)";
}

const char* skybox_pixel_shader_source()
{
    return R"(
Texture2D skybox_texture : register(t0);
SamplerState skybox_sampler : register(s0);

struct PSInput
{
    float4 position : SV_POSITION;
    float2 texcoord : TEXCOORD0;
};

float4 main(PSInput input) : SV_TARGET
{
    const float4 texel = skybox_texture.Sample(skybox_sampler, input.texcoord);
    return float4(texel.rgb, 1.0f);
}
)";
}
}
