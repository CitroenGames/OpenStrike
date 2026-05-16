#define MATERIAL_FLAG_TRANSLUCENT 1u
#define MATERIAL_FLAG_ALPHA_TEST 2u
#define MATERIAL_FLAG_UNLIT 4u
#define MATERIAL_FLAG_NO_DRAW 8u

cbuffer WorldConstants : register(b0)
{
    row_major float4x4 world_to_clip;
    float4 world_light_direction_ambient;
    float4 world_light_color_intensity;
    float4 forward_plus_params; // tile_size, tile_count_x, tile_count_y, light_count
};

Texture2D world_texture : register(t0);
Texture2D world_lightmap : register(t1);
SamplerState world_sampler : register(s0);
SamplerState world_lightmap_sampler : register(s1);

struct ForwardPlusTile
{
    uint light_offset;
    uint light_count;
    uint padding0;
    uint padding1;
};

struct ForwardPlusLight
{
    float4 position_radius;
    float4 color_intensity;
};

StructuredBuffer<ForwardPlusTile> forward_plus_tiles : register(t2);
StructuredBuffer<uint> forward_plus_light_indices : register(t3);
StructuredBuffer<ForwardPlusLight> forward_plus_lights : register(t4);

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
    float2 lightmap_texcoord : TEXCOORD1;
    float lightmap_weight : TEXCOORD2;
};

struct PSInput
{
    float4 position : SV_POSITION;
    float2 texcoord : TEXCOORD0;
    float2 lightmap_texcoord : TEXCOORD1;
    float3 world_position : TEXCOORD2;
    float lightmap_weight : TEXCOORD3;
    float3 normal : NORMAL;
};

float SrgbToLinearChannel(float value)
{
    value = saturate(value);
    if (value <= 0.04045f)
    {
        return value / 12.92f;
    }
    return pow((value + 0.055f) / 1.055f, 2.4f);
}

float3 SrgbToLinear(float3 value)
{
    return float3(
        SrgbToLinearChannel(value.r),
        SrgbToLinearChannel(value.g),
        SrgbToLinearChannel(value.b));
}

float LinearToSrgbChannel(float value)
{
    value = max(value, 0.0f);
    if (value <= 0.0031308f)
    {
        return value * 12.92f;
    }
    return (1.055f * pow(value, 1.0f / 2.4f)) - 0.055f;
}

float3 LinearToSrgb(float3 value)
{
    return float3(
        LinearToSrgbChannel(value.r),
        LinearToSrgbChannel(value.g),
        LinearToSrgbChannel(value.b));
}

PSInput VSMain(VSInput input)
{
    PSInput output;
    output.position = mul(float4(input.position, 1.0f), world_to_clip);
    output.world_position = input.position;
    output.normal = input.normal;
    output.texcoord = input.texcoord;
    output.lightmap_texcoord = input.lightmap_texcoord;
    output.lightmap_weight = input.lightmap_weight;
    return output;
}

float3 ForwardPlusLighting(float3 world_position, float3 normal, float4 screen_position)
{
    const uint light_count = (uint)forward_plus_params.w;
    if (light_count == 0u)
    {
        return float3(0.0f, 0.0f, 0.0f);
    }

    const uint tile_size = max((uint)forward_plus_params.x, 1u);
    const uint tile_count_x = max((uint)forward_plus_params.y, 1u);
    const uint tile_count_y = max((uint)forward_plus_params.z, 1u);
    const uint tile_x = min((uint)(screen_position.x / (float)tile_size), tile_count_x - 1u);
    const uint tile_y = min((uint)(screen_position.y / (float)tile_size), tile_count_y - 1u);
    const uint tile_index = min((tile_y * tile_count_x) + tile_x, (tile_count_x * tile_count_y) - 1u);
    const ForwardPlusTile tile = forward_plus_tiles[tile_index];

    float3 lighting = float3(0.0f, 0.0f, 0.0f);
    [loop]
    for (uint index = 0u; index < tile.light_count; ++index)
    {
        const uint light_index = forward_plus_light_indices[tile.light_offset + index];
        if (light_index >= light_count)
        {
            continue;
        }

        const ForwardPlusLight light = forward_plus_lights[light_index];
        const float3 to_light = light.position_radius.xyz - world_position;
        const float distance_sq = dot(to_light, to_light);
        const float radius = max(light.position_radius.w, 1.0f);
        const float radius_sq = radius * radius;
        if (distance_sq >= radius_sq)
        {
            continue;
        }

        const float distance_to_light = sqrt(max(distance_sq, 0.0001f));
        const float3 light_direction = to_light / distance_to_light;
        const float attenuation = saturate(1.0f - (distance_to_light / radius));
        const float diffuse = saturate(dot(normal, light_direction)) * attenuation * attenuation;
        lighting += light.color_intensity.rgb * light.color_intensity.w * diffuse;
    }

    return lighting;
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

    const float3 base_color = SrgbToLinear(texel.rgb) * saturate(material_base_color.rgb);
    if ((material_flags & MATERIAL_FLAG_UNLIT) != 0u)
    {
        return float4(saturate(LinearToSrgb(base_color)), alpha);
    }

    const float3 normal = normalize(input.normal);
    const float3 light_direction = normalize(world_light_direction_ambient.xyz);
    const float ambient = saturate(world_light_direction_ambient.w);
    const float diffuse = saturate(dot(normal, light_direction)) * max(world_light_color_intensity.w, 0.0f);
    const float3 forward_plus_lighting = ForwardPlusLighting(input.world_position, normal, input.position);
    const float3 fallback_lighting = ambient + (world_light_color_intensity.rgb * diffuse);
    const float3 baked_lighting = world_lightmap.Sample(world_lightmap_sampler, input.lightmap_texcoord).rgb;
    const float3 static_lighting = lerp(fallback_lighting, baked_lighting, saturate(input.lightmap_weight));
    const float3 lit_color = base_color * (static_lighting + forward_plus_lighting);
    return float4(saturate(LinearToSrgb(lit_color)), alpha);
}
