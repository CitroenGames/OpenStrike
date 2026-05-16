#include "openstrike/renderer/metal_renderer.hpp"

#include "openstrike/core/log.hpp"
#include "openstrike/engine/engine_context.hpp"
#include "openstrike/engine/sdl_input.hpp"
#include "openstrike/material/material_system.hpp"
#include "openstrike/source/source_asset_store.hpp"
#include "openstrike/source/source_texture.hpp"
#include "openstrike/ui/rml_ui_layer.hpp"
#include "openstrike/world/world.hpp"

#include <RmlUi/Core.h>
#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>
#include <SDL3/SDL_metal.h>

#import <CoreGraphics/CoreGraphics.h>
#import <ImageIO/ImageIO.h>
#import <Metal/Metal.h>
#import <QuartzCore/CAMetalLayer.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cctype>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <format>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace openstrike
{
namespace
{
constexpr const char* kWindowTitle = "OpenStrike - Metal";
constexpr MTLPixelFormat kRenderTargetFormat = MTLPixelFormatBGRA8Unorm;
constexpr MTLPixelFormat kDepthFormat = MTLPixelFormatDepth32Float;
constexpr float kPi = 3.14159265358979323846F;
constexpr std::uint32_t kSkyboxFaceCount = 6;
constexpr std::uint32_t kMetalMaxLights = 128;
constexpr std::size_t kWorldTransformFloatCount = 16;
constexpr std::size_t kWorldShaderFloatCount = 28;

struct WorldDrawVertex
{
    float position[3]{};
    float normal[3]{};
    float texcoord[2]{};
    float lightmap_texcoord[2]{};
    float lightmap_weight = 0.0F;
};

struct SkyboxDrawVertex
{
    float position[3]{};
    float texcoord[2]{};
};

struct WorldDrawBatch
{
    std::uint32_t material_index = 0;
    std::uint32_t first_index = 0;
    std::uint32_t index_count = 0;
};

struct MetalLightGpu
{
    float position_radius[4]{};
    float color_intensity[4]{};
};

struct WorldGpuResources
{
    id<MTLBuffer> vertex_buffer = nil;
    id<MTLBuffer> index_buffer = nil;
    id<MTLTexture> lightmap_texture = nil;
    std::vector<id<MTLTexture>> textures;
    std::vector<MaterialGpuConstants> material_constants;
    std::vector<WorldDrawBatch> batches;
    std::uint64_t generation = 0;
    std::uint32_t index_count = 0;
    std::uint32_t loaded_texture_count = 0;
    std::uint32_t fallback_texture_count = 0;
};

struct SkyboxGpuResources
{
    id<MTLBuffer> vertex_buffer = nil;
    id<MTLBuffer> index_buffer = nil;
    std::array<id<MTLTexture>, kSkyboxFaceCount> textures{};
    std::string sky_name;
    std::uint64_t generation = 0;
};

struct MetalFrameLights
{
    id<MTLBuffer> buffer = nil;
    std::uint32_t light_count = 0;
};

MTLClearColor frame_clear_color(std::uint64_t frame_index)
{
    const double pulse = static_cast<double>(frame_index % 120U) / 120.0;
    return MTLClearColorMake(0.03, 0.05 + (0.05 * pulse), 0.09 + (0.15 * pulse), 1.0);
}

float degrees_to_radians(float degrees)
{
    return degrees * (kPi / 180.0F);
}

float dot(Vec3 lhs, Vec3 rhs)
{
    return (lhs.x * rhs.x) + (lhs.y * rhs.y) + (lhs.z * rhs.z);
}

Vec3 cross(Vec3 lhs, Vec3 rhs)
{
    return {
        (lhs.y * rhs.z) - (lhs.z * rhs.y),
        (lhs.z * rhs.x) - (lhs.x * rhs.z),
        (lhs.x * rhs.y) - (lhs.y * rhs.x),
    };
}

Vec3 normalize(Vec3 value)
{
    const float length = std::sqrt(dot(value, value));
    if (length <= 0.00001F)
    {
        return {};
    }

    return {value.x / length, value.y / length, value.z / length};
}

std::array<float, 16> multiply_matrix(std::array<float, 16> lhs, std::array<float, 16> rhs)
{
    std::array<float, 16> result{};
    for (std::size_t row = 0; row < 4; ++row)
    {
        for (std::size_t column = 0; column < 4; ++column)
        {
            for (std::size_t index = 0; index < 4; ++index)
            {
                result[(row * 4) + column] += lhs[(row * 4) + index] * rhs[(index * 4) + column];
            }
        }
    }
    return result;
}

Vec3 camera_forward_2d(float yaw_degrees)
{
    const float yaw_radians = degrees_to_radians(yaw_degrees);
    return normalize({std::cos(yaw_radians), std::sin(yaw_radians), 0.0F});
}

Vec3 camera_right_2d(float yaw_degrees)
{
    const float yaw_radians = degrees_to_radians(yaw_degrees);
    return normalize({std::sin(yaw_radians), -std::cos(yaw_radians), 0.0F});
}

std::array<float, 16> camera_world_to_clip_matrix(const CameraState& camera, std::uint32_t width, std::uint32_t height)
{
    const Vec3 horizontal_forward = camera_forward_2d(camera.yaw_degrees);
    const float pitch_radians = degrees_to_radians(camera.pitch_degrees);
    const float cos_pitch = std::cos(pitch_radians);
    const float sin_pitch = std::sin(pitch_radians);
    const Vec3 forward = normalize({horizontal_forward.x * cos_pitch, horizontal_forward.y * cos_pitch, -sin_pitch});
    const Vec3 right = camera_right_2d(camera.yaw_degrees);
    const Vec3 up = normalize(cross(right, forward));
    const Vec3 eye = camera.origin;

    const std::array<float, 16> view{
        right.x,
        up.x,
        forward.x,
        0.0F,
        right.y,
        up.y,
        forward.y,
        0.0F,
        right.z,
        up.z,
        forward.z,
        0.0F,
        -dot(eye, right),
        -dot(eye, up),
        -dot(eye, forward),
        1.0F,
    };

    const float aspect = height > 0 ? static_cast<float>(width) / static_cast<float>(height) : 1.0F;
    const float fov_radians = degrees_to_radians(std::clamp(camera.fov_degrees, 30.0F, 130.0F));
    const float y_scale = 1.0F / std::tan(fov_radians * 0.5F);
    const float x_scale = y_scale / std::max(0.1F, aspect);
    constexpr float near_z = 4.0F;
    constexpr float far_z = 65536.0F;
    const float depth_scale = far_z / (far_z - near_z);

    const std::array<float, 16> projection{
        x_scale,
        0.0F,
        0.0F,
        0.0F,
        0.0F,
        y_scale,
        0.0F,
        0.0F,
        0.0F,
        0.0F,
        depth_scale,
        1.0F,
        0.0F,
        0.0F,
        -near_z * depth_scale,
        0.0F,
    };

    return multiply_matrix(view, projection);
}

std::array<float, 16> skybox_to_clip_matrix(const CameraState* camera, std::uint32_t width, std::uint32_t height)
{
    const float yaw_degrees = camera != nullptr && camera->active ? camera->yaw_degrees : 0.0F;
    const float pitch_degrees = camera != nullptr && camera->active ? camera->pitch_degrees : 0.0F;
    const Vec3 horizontal_forward = camera_forward_2d(yaw_degrees);
    const float pitch_radians = degrees_to_radians(pitch_degrees);
    const float cos_pitch = std::cos(pitch_radians);
    const float sin_pitch = std::sin(pitch_radians);
    const Vec3 forward = normalize({horizontal_forward.x * cos_pitch, horizontal_forward.y * cos_pitch, -sin_pitch});
    const Vec3 right = camera_right_2d(yaw_degrees);
    const Vec3 up = normalize(cross(right, forward));

    const std::array<float, 16> view{
        right.x,
        up.x,
        forward.x,
        0.0F,
        right.y,
        up.y,
        forward.y,
        0.0F,
        right.z,
        up.z,
        forward.z,
        0.0F,
        0.0F,
        0.0F,
        0.0F,
        1.0F,
    };

    const float aspect = height > 0 ? static_cast<float>(width) / static_cast<float>(height) : 1.0F;
    const float fov_radians =
        degrees_to_radians(camera != nullptr && camera->active ? std::clamp(camera->fov_degrees, 30.0F, 130.0F) : 75.0F);
    const float y_scale = 1.0F / std::tan(fov_radians * 0.5F);
    const float x_scale = y_scale / std::max(0.1F, aspect);
    constexpr float near_z = 0.01F;
    constexpr float far_z = 16.0F;
    const float depth_scale = far_z / (far_z - near_z);

    const std::array<float, 16> projection{
        x_scale,
        0.0F,
        0.0F,
        0.0F,
        0.0F,
        y_scale,
        0.0F,
        0.0F,
        0.0F,
        0.0F,
        depth_scale,
        1.0F,
        0.0F,
        0.0F,
        -near_z * depth_scale,
        0.0F,
    };

    return multiply_matrix(view, projection);
}

std::array<float, 16> overview_world_to_clip_matrix(const WorldMesh& mesh, std::uint32_t width, std::uint32_t height)
{
    if (!mesh.has_bounds)
    {
        return {1.0F, 0.0F, 0.0F, 0.0F, 0.0F, 1.0F, 0.0F, 0.0F, 0.0F, 0.0F, 1.0F, 0.0F, 0.0F, 0.0F, 0.0F, 1.0F};
    }

    const float extent_x = std::max(1.0F, mesh.bounds_max.x - mesh.bounds_min.x);
    const float extent_y = std::max(1.0F, mesh.bounds_max.y - mesh.bounds_min.y);
    const float extent_z = std::max(1.0F, mesh.bounds_max.z - mesh.bounds_min.z);
    const float aspect = height > 0 ? static_cast<float>(width) / static_cast<float>(height) : 1.0F;
    float half_width = std::max(extent_x * 0.58F, extent_y * 0.58F * aspect);
    float half_height = half_width / std::max(0.1F, aspect);
    half_width = std::max(half_width, 64.0F);
    half_height = std::max(half_height, 64.0F);

    const float center_x = (mesh.bounds_min.x + mesh.bounds_max.x) * 0.5F;
    const float center_y = (mesh.bounds_min.y + mesh.bounds_max.y) * 0.5F;
    const float max_z = mesh.bounds_max.z + 64.0F;
    const float depth_range = extent_z + 128.0F;

    return {
        1.0F / half_width,
        0.0F,
        0.0F,
        0.0F,
        0.0F,
        -1.0F / half_height,
        0.0F,
        0.0F,
        0.0F,
        0.0F,
        -1.0F / depth_range,
        0.0F,
        -center_x / half_width,
        center_y / half_height,
        max_z / depth_range,
        1.0F,
    };
}

std::array<float, 16> world_to_clip_matrix(const WorldMesh& mesh, const CameraState* camera, std::uint32_t width, std::uint32_t height)
{
    if (camera != nullptr && camera->active)
    {
        return camera_world_to_clip_matrix(*camera, width, height);
    }

    return overview_world_to_clip_matrix(mesh, width, height);
}

std::array<float, kWorldShaderFloatCount> world_shader_constants(
    const WorldMesh& mesh,
    const CameraState* camera,
    std::uint32_t width,
    std::uint32_t height,
    std::uint32_t light_count)
{
    std::array<float, kWorldShaderFloatCount> constants{};
    const std::array<float, 16> transform = world_to_clip_matrix(mesh, camera, width, height);
    std::copy(transform.begin(), transform.end(), constants.begin());

    const Vec3 light_direction = normalize({-0.35F, -0.45F, 0.82F});
    constants[kWorldTransformFloatCount + 0] = light_direction.x;
    constants[kWorldTransformFloatCount + 1] = light_direction.y;
    constants[kWorldTransformFloatCount + 2] = light_direction.z;
    constants[kWorldTransformFloatCount + 3] = 0.36F;
    constants[kWorldTransformFloatCount + 4] = 1.0F;
    constants[kWorldTransformFloatCount + 5] = 0.96F;
    constants[kWorldTransformFloatCount + 6] = 0.88F;
    constants[kWorldTransformFloatCount + 7] = 0.78F;
    constants[kWorldTransformFloatCount + 8] = 16.0F;
    constants[kWorldTransformFloatCount + 9] = static_cast<float>((width + 15U) / 16U);
    constants[kWorldTransformFloatCount + 10] = static_cast<float>((height + 15U) / 16U);
    constants[kWorldTransformFloatCount + 11] = static_cast<float>(light_count);
    return constants;
}

SkyboxDrawVertex make_skybox_vertex(float s, float t, std::uint32_t axis)
{
    static constexpr std::array<std::array<int, 3>, kSkyboxFaceCount> kSourceSkyStToVec{{
        {3, -1, 2},
        {-3, 1, 2},
        {1, 3, 2},
        {-1, -3, 2},
        {-2, -1, 3},
        {2, -1, -3},
    }};

    const std::array<float, 3> basis{s, t, 1.0F};
    SkyboxDrawVertex vertex{};
    for (std::size_t component = 0; component < 3; ++component)
    {
        const int mapping = kSourceSkyStToVec[axis][component];
        const float sign = mapping < 0 ? -1.0F : 1.0F;
        const std::size_t source = static_cast<std::size_t>(std::abs(mapping) - 1);
        vertex.position[component] = basis[source] * sign;
    }

    vertex.texcoord[0] = (s + 1.0F) * 0.5F;
    vertex.texcoord[1] = 1.0F - ((t + 1.0F) * 0.5F);
    return vertex;
}

std::array<SkyboxDrawVertex, kSkyboxFaceCount * 4> build_skybox_vertices()
{
    std::array<SkyboxDrawVertex, kSkyboxFaceCount * 4> vertices{};
    for (std::uint32_t face = 0; face < kSkyboxFaceCount; ++face)
    {
        const std::size_t base = static_cast<std::size_t>(face) * 4;
        vertices[base + 0] = make_skybox_vertex(-1.0F, -1.0F, face);
        vertices[base + 1] = make_skybox_vertex(-1.0F, 1.0F, face);
        vertices[base + 2] = make_skybox_vertex(1.0F, 1.0F, face);
        vertices[base + 3] = make_skybox_vertex(1.0F, -1.0F, face);
    }
    return vertices;
}

std::array<std::uint16_t, kSkyboxFaceCount * 6> build_skybox_indices()
{
    std::array<std::uint16_t, kSkyboxFaceCount * 6> indices{};
    for (std::uint16_t face = 0; face < kSkyboxFaceCount; ++face)
    {
        const std::uint16_t vertex = static_cast<std::uint16_t>(face * 4);
        const std::size_t index = static_cast<std::size_t>(face) * 6;
        indices[index + 0] = vertex;
        indices[index + 1] = static_cast<std::uint16_t>(vertex + 1);
        indices[index + 2] = static_cast<std::uint16_t>(vertex + 2);
        indices[index + 3] = vertex;
        indices[index + 4] = static_cast<std::uint16_t>(vertex + 2);
        indices[index + 5] = static_cast<std::uint16_t>(vertex + 3);
    }
    return indices;
}

std::string lower_ascii_copy(std::string_view text)
{
    std::string result(text);
    std::transform(result.begin(), result.end(), result.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    return result;
}

std::string_view trim_ascii(std::string_view text)
{
    while (!text.empty() && std::isspace(static_cast<unsigned char>(text.front())) != 0)
    {
        text.remove_prefix(1);
    }
    while (!text.empty() && std::isspace(static_cast<unsigned char>(text.back())) != 0)
    {
        text.remove_suffix(1);
    }
    return text;
}

bool remove_prefix(std::string& value, std::string_view prefix)
{
    if (value.rfind(prefix, 0) != 0)
    {
        return false;
    }

    value.erase(0, prefix.size());
    return true;
}

void remove_suffix(std::string& value, std::string_view suffix)
{
    if (value.size() >= suffix.size() && value.compare(value.size() - suffix.size(), suffix.size(), suffix) == 0)
    {
        value.resize(value.size() - suffix.size());
    }
}

std::string normalize_skybox_name(std::string_view sky_name)
{
    std::string normalized = lower_ascii_copy(trim_ascii(sky_name));
    std::replace(normalized.begin(), normalized.end(), '\\', '/');
    remove_prefix(normalized, "materials/");
    remove_prefix(normalized, "skybox/");
    remove_suffix(normalized, ".vmt");
    remove_suffix(normalized, ".vtf");
    return normalized;
}

std::optional<std::string> skybox_name_from_world(const LoadedWorld& world)
{
    for (std::string_view key : {"skyname", "sv_skyname", "sky_name"})
    {
        if (const auto it = world.worldspawn.find(std::string(key)); it != world.worldspawn.end())
        {
            std::string sky_name = normalize_skybox_name(it->second);
            if (!sky_name.empty())
            {
                return sky_name;
            }
        }
    }

    if (world.kind == WorldAssetKind::SourceBsp && world.mesh.has_sky_surfaces)
    {
        return std::string("sky_urb01");
    }

    return std::nullopt;
}

MetalLightGpu metal_light_gpu(const WorldLight& light)
{
    MetalLightGpu gpu{};
    gpu.position_radius[0] = light.position.x;
    gpu.position_radius[1] = light.position.y;
    gpu.position_radius[2] = light.position.z;
    gpu.position_radius[3] = light.radius;
    gpu.color_intensity[0] = light.color[0];
    gpu.color_intensity[1] = light.color[1];
    gpu.color_intensity[2] = light.color[2];
    gpu.color_intensity[3] = light.intensity;
    return gpu;
}

const char* metal_shader_source()
{
    return R"MSL(
#include <metal_stdlib>
using namespace metal;

#define MATERIAL_FLAG_ALPHA_TEST 2u
#define MATERIAL_FLAG_UNLIT 4u
#define MATERIAL_FLAG_NO_DRAW 8u

struct WorldDrawVertex
{
    packed_float3 position;
    packed_float3 normal;
    packed_float2 texcoord;
    packed_float2 lightmap_texcoord;
    float lightmap_weight;
};

struct SkyboxDrawVertex
{
    packed_float3 position;
    packed_float2 texcoord;
};

struct WorldConstants
{
    float transform[16];
    float4 world_light_direction_ambient;
    float4 world_light_color_intensity;
    float4 light_params;
};

struct MaterialConstants
{
    float4 base_color;
    uint flags;
    float alpha_cutoff;
    float2 padding;
};

struct MetalLight
{
    float4 position_radius;
    float4 color_intensity;
};

struct WorldVsOut
{
    float4 position [[position]];
    float2 texcoord;
    float2 lightmap_texcoord;
    float3 world_position;
    float lightmap_weight;
    float3 normal;
};

struct SkyboxVsOut
{
    float4 position [[position]];
    float2 texcoord;
};

float4 transform_position(float3 position, constant float* m)
{
    return float4(
        (position.x * m[0]) + (position.y * m[4]) + (position.z * m[8]) + m[12],
        (position.x * m[1]) + (position.y * m[5]) + (position.z * m[9]) + m[13],
        (position.x * m[2]) + (position.y * m[6]) + (position.z * m[10]) + m[14],
        (position.x * m[3]) + (position.y * m[7]) + (position.z * m[11]) + m[15]);
}

float srgb_to_linear_channel(float value)
{
    value = saturate(value);
    if (value <= 0.04045f)
    {
        return value / 12.92f;
    }
    return pow((value + 0.055f) / 1.055f, 2.4f);
}

float3 srgb_to_linear(float3 value)
{
    return float3(
        srgb_to_linear_channel(value.r),
        srgb_to_linear_channel(value.g),
        srgb_to_linear_channel(value.b));
}

float linear_to_srgb_channel(float value)
{
    value = max(value, 0.0f);
    if (value <= 0.0031308f)
    {
        return value * 12.92f;
    }
    return (1.055f * pow(value, 1.0f / 2.4f)) - 0.055f;
}

float3 linear_to_srgb(float3 value)
{
    return float3(
        linear_to_srgb_channel(value.r),
        linear_to_srgb_channel(value.g),
        linear_to_srgb_channel(value.b));
}

vertex WorldVsOut world_vertex_main(
    uint vertex_id [[vertex_id]],
    device const WorldDrawVertex* vertices [[buffer(0)]],
    constant WorldConstants& constants [[buffer(1)]])
{
    const WorldDrawVertex input = vertices[vertex_id];
    WorldVsOut output;
    output.position = transform_position(float3(input.position), constants.transform);
    output.world_position = float3(input.position);
    output.normal = float3(input.normal);
    output.texcoord = input.texcoord;
    output.lightmap_texcoord = input.lightmap_texcoord;
    output.lightmap_weight = input.lightmap_weight;
    return output;
}

float3 dynamic_lighting(
    float3 world_position,
    float3 normal,
    constant WorldConstants& constants,
    device const MetalLight* lights)
{
    const uint light_count = min((uint)constants.light_params.w, 128u);
    float3 lighting = float3(0.0f);
    for (uint index = 0u; index < light_count; ++index)
    {
        const MetalLight light = lights[index];
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

fragment float4 world_fragment_main(
    WorldVsOut input [[stage_in]],
    constant MaterialConstants& material [[buffer(0)]],
    constant WorldConstants& constants [[buffer(1)]],
    device const MetalLight* lights [[buffer(2)]],
    texture2d<float> world_texture [[texture(0)]],
    texture2d<float> world_lightmap [[texture(1)]],
    sampler world_sampler [[sampler(0)]],
    sampler lightmap_sampler [[sampler(1)]])
{
    if ((material.flags & MATERIAL_FLAG_NO_DRAW) != 0u)
    {
        discard_fragment();
    }

    const float4 texel = world_texture.sample(world_sampler, input.texcoord);
    const float alpha = saturate(texel.a * material.base_color.a);
    if ((material.flags & MATERIAL_FLAG_ALPHA_TEST) != 0u && alpha < material.alpha_cutoff)
    {
        discard_fragment();
    }

    const float3 base_color = srgb_to_linear(texel.rgb) * saturate(material.base_color.rgb);
    if ((material.flags & MATERIAL_FLAG_UNLIT) != 0u)
    {
        return float4(saturate(linear_to_srgb(base_color)), alpha);
    }

    const float3 normal = normalize(input.normal);
    const float3 light_direction = normalize(constants.world_light_direction_ambient.xyz);
    const float ambient = saturate(constants.world_light_direction_ambient.w);
    const float diffuse = saturate(dot(normal, light_direction)) * max(constants.world_light_color_intensity.w, 0.0f);
    const float3 fallback_lighting = ambient + (constants.world_light_color_intensity.rgb * diffuse);
    const float3 baked_lighting = world_lightmap.sample(lightmap_sampler, input.lightmap_texcoord).rgb;
    const float3 static_lighting = mix(fallback_lighting, baked_lighting, saturate(input.lightmap_weight));
    const float3 lit_color = base_color * (static_lighting + dynamic_lighting(input.world_position, normal, constants, lights));
    return float4(saturate(linear_to_srgb(lit_color)), alpha);
}

vertex SkyboxVsOut skybox_vertex_main(
    uint vertex_id [[vertex_id]],
    device const SkyboxDrawVertex* vertices [[buffer(0)]],
    constant float* transform [[buffer(1)]])
{
    const SkyboxDrawVertex input = vertices[vertex_id];
    SkyboxVsOut output;
    output.position = transform_position(float3(input.position), transform);
    output.texcoord = input.texcoord;
    return output;
}

fragment float4 skybox_fragment_main(
    SkyboxVsOut input [[stage_in]],
    texture2d<float> skybox_texture [[texture(0)]],
    sampler skybox_sampler [[sampler(0)]])
{
    const float4 texel = skybox_texture.sample(skybox_sampler, input.texcoord);
    return float4(texel.rgb, 1.0f);
}
)MSL";
}

id<MTLBuffer> create_buffer(id<MTLDevice> device, const void* data, std::size_t byte_count, NSString* label)
{
    if (device == nil || data == nullptr || byte_count == 0)
    {
        return nil;
    }

    id<MTLBuffer> buffer = [device newBufferWithBytes:data length:byte_count options:MTLResourceStorageModeShared];
    if (buffer != nil)
    {
        buffer.label = label;
    }
    return buffer;
}

id<MTLTexture> upload_source_texture_to_metal(id<MTLDevice> device, const SourceTexture& texture, NSString* label)
{
    if (device == nil || texture.mips.empty())
    {
        return nil;
    }

    std::optional<SourceTexture> rgba_texture = source_texture_to_rgba8(texture);
    if (!rgba_texture || rgba_texture->mips.empty())
    {
        return nil;
    }

    MTLTextureDescriptor* descriptor = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm
                                                                                           width:std::max<std::uint32_t>(rgba_texture->width, 1U)
                                                                                          height:std::max<std::uint32_t>(rgba_texture->height, 1U)
                                                                                       mipmapped:rgba_texture->mips.size() > 1];
    descriptor.usage = MTLTextureUsageShaderRead;
    descriptor.storageMode = MTLStorageModeShared;

    id<MTLTexture> output = [device newTextureWithDescriptor:descriptor];
    if (output == nil)
    {
        return nil;
    }
    output.label = label;

    for (std::size_t mip = 0; mip < rgba_texture->mips.size(); ++mip)
    {
        const SourceTextureMip& source_mip = rgba_texture->mips[mip];
        const std::uint32_t width = std::max<std::uint32_t>(source_mip.width, 1U);
        const std::uint32_t height = std::max<std::uint32_t>(source_mip.height, 1U);
        const std::uint32_t row_bytes = source_texture_row_bytes(SourceTextureFormat::Rgba8, width);
        const std::uint64_t required_bytes = static_cast<std::uint64_t>(row_bytes) * height;
        if (source_mip.bytes.size() < required_bytes)
        {
            return nil;
        }

        MTLRegion region = MTLRegionMake2D(0, 0, width, height);
        [output replaceRegion:region mipmapLevel:static_cast<NSUInteger>(mip) withBytes:source_mip.bytes.data() bytesPerRow:row_bytes];
    }

    return output;
}

id<MTLTexture> upload_lightmap_to_metal(id<MTLDevice> device, const WorldLightmapAtlas& atlas, NSString* label)
{
    if (device == nil)
    {
        return nil;
    }

    const std::uint32_t width = std::max<std::uint32_t>(atlas.width, 1U);
    const std::uint32_t height = std::max<std::uint32_t>(atlas.height, 1U);
    const std::size_t required_floats = static_cast<std::size_t>(width) * height * 4U;
    std::vector<float> fallback_pixels;
    const float* pixels = atlas.rgba.size() >= required_floats ? atlas.rgba.data() : nullptr;
    if (pixels == nullptr)
    {
        fallback_pixels.assign(required_floats, 1.0F);
        pixels = fallback_pixels.data();
    }

    MTLTextureDescriptor* descriptor = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA32Float
                                                                                           width:width
                                                                                          height:height
                                                                                       mipmapped:NO];
    descriptor.usage = MTLTextureUsageShaderRead;
    descriptor.storageMode = MTLStorageModeShared;

    id<MTLTexture> output = [device newTextureWithDescriptor:descriptor];
    if (output == nil)
    {
        return nil;
    }
    output.label = label;

    MTLRegion region = MTLRegionMake2D(0, 0, width, height);
    [output replaceRegion:region mipmapLevel:0 withBytes:pixels bytesPerRow:static_cast<NSUInteger>(width) * 4U * sizeof(float)];
    return output;
}

void premultiply_alpha(std::vector<Rml::byte>& pixels)
{
    for (std::size_t index = 0; index + 3 < pixels.size(); index += 4)
    {
        const std::uint32_t alpha = pixels[index + 3];
        pixels[index + 0] = static_cast<Rml::byte>((static_cast<std::uint32_t>(pixels[index + 0]) * alpha) / 255U);
        pixels[index + 1] = static_cast<Rml::byte>((static_cast<std::uint32_t>(pixels[index + 1]) * alpha) / 255U);
        pixels[index + 2] = static_cast<Rml::byte>((static_cast<std::uint32_t>(pixels[index + 2]) * alpha) / 255U);
    }
}

std::vector<Rml::byte> make_go_button_placeholder(std::uint32_t texture_width, std::uint32_t texture_height)
{
    std::vector<Rml::byte> pixels(static_cast<std::size_t>(texture_width) * texture_height * 4U);
    for (std::uint32_t y = 0; y < texture_height; ++y)
    {
        for (std::uint32_t x = 0; x < texture_width; ++x)
        {
            const float u = static_cast<float>(x) / static_cast<float>(std::max(1U, texture_width - 1U));
            const auto index = static_cast<std::size_t>((y * texture_width) + x) * 4U;
            pixels[index + 0] = static_cast<Rml::byte>(70.0F + (80.0F * u));
            pixels[index + 1] = static_cast<Rml::byte>(150.0F + (70.0F * u));
            pixels[index + 2] = static_cast<Rml::byte>(72.0F + (30.0F * u));
            pixels[index + 3] = 255;
        }
    }
    return pixels;
}

bool checked_texture_byte_size(std::uint32_t width, std::uint32_t height, std::size_t& out_byte_size)
{
    if (width == 0 || height == 0)
    {
        return false;
    }

    const auto pixel_count = static_cast<std::uint64_t>(width) * static_cast<std::uint64_t>(height);
    const auto byte_count = pixel_count * 4ULL;
    if (byte_count > static_cast<std::uint64_t>(std::numeric_limits<std::size_t>::max()))
    {
        return false;
    }

    out_byte_size = static_cast<std::size_t>(byte_count);
    return true;
}

std::string normalize_texture_source(std::string source)
{
    std::replace(source.begin(), source.end(), '\\', '/');
    if (source.rfind("file:///", 0) == 0)
    {
        source.erase(0, 7);
    }
    else if (source.rfind("file://", 0) == 0)
    {
        source.erase(0, 7);
    }
    return source;
}

bool is_video_texture_source(std::string_view normalized_source)
{
    const std::string lower = lower_ascii_copy(normalized_source);
    return lower.find("/__video/") != std::string::npos || lower.ends_with(".webm");
}

struct RmlMetalVertex
{
    float position[2]{};
    float color[4]{};
    float texcoord[2]{};
};

struct RmlMetalGeometry
{
    std::vector<Rml::Vertex> vertices;
    std::vector<int> indices;
};

struct RmlMetalTexture
{
    id<MTLTexture> texture = nil;
    Rml::Vector2i dimensions{};
};

const char* rml_metal_shader_source()
{
    return R"MSL(
#include <metal_stdlib>
using namespace metal;

struct RmlVertex
{
    packed_float2 position;
    packed_float4 color;
    packed_float2 texcoord;
};

struct RmlVsOut
{
    float4 position [[position]];
    float4 color;
    float2 texcoord;
};

vertex RmlVsOut rml_vertex_main(
    uint vertex_id [[vertex_id]],
    device const RmlVertex* vertices [[buffer(0)]],
    constant float2& viewport_size [[buffer(1)]])
{
    const RmlVertex input = vertices[vertex_id];
    const float2 ndc = float2((input.position.x / viewport_size.x) * 2.0f - 1.0f,
                              1.0f - (input.position.y / viewport_size.y) * 2.0f);
    RmlVsOut output;
    output.position = float4(ndc, 0.0f, 1.0f);
    output.color = input.color;
    output.texcoord = input.texcoord;
    return output;
}

fragment float4 rml_fragment_main(
    RmlVsOut input [[stage_in]],
    texture2d<float> rml_texture [[texture(0)]],
    sampler rml_sampler [[sampler(0)]])
{
    const float4 texel = rml_texture.sample(rml_sampler, input.texcoord);
    return float4(texel.rgb * input.color.rgb, texel.a * input.color.a);
}
)MSL";
}

class RmlMetalRenderInterface final : public Rml::RenderInterface
{
public:
    bool initialize(id<MTLDevice> in_device)
    {
        device_ = in_device;
        if (device_ == nil)
        {
            return false;
        }

        NSError* error = nil;
        library_ = [device_ newLibraryWithSource:[NSString stringWithUTF8String:rml_metal_shader_source()] options:nil error:&error];
        if (library_ == nil)
        {
            log_error("RmlUi Metal shader compile failed: {}", error != nil ? [[error localizedDescription] UTF8String] : "unknown error");
            return false;
        }

        id<MTLFunction> vertex = [library_ newFunctionWithName:@"rml_vertex_main"];
        id<MTLFunction> fragment = [library_ newFunctionWithName:@"rml_fragment_main"];
        if (vertex == nil || fragment == nil)
        {
            log_error("RmlUi Metal shader library is missing entry points");
            return false;
        }

        MTLRenderPipelineDescriptor* descriptor = [[MTLRenderPipelineDescriptor alloc] init];
        descriptor.label = @"OpenStrike RmlUi Pipeline";
        descriptor.vertexFunction = vertex;
        descriptor.fragmentFunction = fragment;
        descriptor.colorAttachments[0].pixelFormat = kRenderTargetFormat;
        descriptor.depthAttachmentPixelFormat = kDepthFormat;
        descriptor.colorAttachments[0].blendingEnabled = YES;
        descriptor.colorAttachments[0].sourceRGBBlendFactor = MTLBlendFactorOne;
        descriptor.colorAttachments[0].destinationRGBBlendFactor = MTLBlendFactorOneMinusSourceAlpha;
        descriptor.colorAttachments[0].rgbBlendOperation = MTLBlendOperationAdd;
        descriptor.colorAttachments[0].sourceAlphaBlendFactor = MTLBlendFactorOne;
        descriptor.colorAttachments[0].destinationAlphaBlendFactor = MTLBlendFactorOneMinusSourceAlpha;
        descriptor.colorAttachments[0].alphaBlendOperation = MTLBlendOperationAdd;
        pipeline_ = [device_ newRenderPipelineStateWithDescriptor:descriptor error:&error];
        if (pipeline_ == nil)
        {
            log_error("failed to create RmlUi Metal pipeline: {}", error != nil ? [[error localizedDescription] UTF8String] : "unknown error");
            return false;
        }

        MTLSamplerDescriptor* sampler_descriptor = [[MTLSamplerDescriptor alloc] init];
        sampler_descriptor.minFilter = MTLSamplerMinMagFilterLinear;
        sampler_descriptor.magFilter = MTLSamplerMinMagFilterLinear;
        sampler_descriptor.mipFilter = MTLSamplerMipFilterNotMipmapped;
        sampler_descriptor.sAddressMode = MTLSamplerAddressModeClampToEdge;
        sampler_descriptor.tAddressMode = MTLSamplerAddressModeClampToEdge;
        sampler_ = [device_ newSamplerStateWithDescriptor:sampler_descriptor];
        if (sampler_ == nil)
        {
            return false;
        }

        const std::array<Rml::byte, 4> white_pixel{255, 255, 255, 255};
        white_texture_ = create_texture(white_pixel.data(), 1, 1, "RmlUi white");
        return white_texture_ != nullptr;
    }

    void shutdown()
    {
        geometries_.clear();
        textures_.clear();
        white_texture_ = nullptr;
        encoder_ = nil;
        pipeline_ = nil;
        sampler_ = nil;
        library_ = nil;
        device_ = nil;
        width_ = 1;
        height_ = 1;
        scissor_enabled_ = false;
        scissor_region_ = Rml::Rectanglei::MakeInvalid();
    }

    void begin_frame(id<MTLRenderCommandEncoder> encoder, std::uint32_t width, std::uint32_t height)
    {
        encoder_ = encoder;
        width_ = std::max<std::uint32_t>(width, 1U);
        height_ = std::max<std::uint32_t>(height, 1U);
    }

    void end_frame()
    {
        encoder_ = nil;
    }

    Rml::CompiledGeometryHandle CompileGeometry(Rml::Span<const Rml::Vertex> vertices, Rml::Span<const int> indices) override
    {
        auto geometry = std::make_unique<RmlMetalGeometry>();
        geometry->vertices.assign(vertices.begin(), vertices.end());
        geometry->indices.assign(indices.begin(), indices.end());
        RmlMetalGeometry* result = geometry.get();
        geometries_.push_back(std::move(geometry));
        return reinterpret_cast<Rml::CompiledGeometryHandle>(result);
    }

    void RenderGeometry(Rml::CompiledGeometryHandle handle, Rml::Vector2f translation, Rml::TextureHandle texture) override
    {
        if (encoder_ == nil || pipeline_ == nil)
        {
            return;
        }

        const auto* geometry = reinterpret_cast<const RmlMetalGeometry*>(handle);
        if (geometry == nullptr || geometry->vertices.empty() || geometry->indices.empty())
        {
            return;
        }

        std::vector<RmlMetalVertex> vertices;
        vertices.reserve(geometry->vertices.size());
        for (const Rml::Vertex& source : geometry->vertices)
        {
            const float color_scale = 1.0F / 255.0F;
            vertices.push_back(RmlMetalVertex{
                {source.position.x + translation.x, source.position.y + translation.y},
                {source.colour.red * color_scale,
                    source.colour.green * color_scale,
                    source.colour.blue * color_scale,
                    source.colour.alpha * color_scale},
                {source.tex_coord.x, source.tex_coord.y},
            });
        }

        std::vector<std::uint32_t> indices;
        indices.reserve(geometry->indices.size());
        for (const int index : geometry->indices)
        {
            if (index >= 0)
            {
                indices.push_back(static_cast<std::uint32_t>(index));
            }
        }
        if (indices.empty())
        {
            return;
        }

        id<MTLBuffer> vertex_buffer = create_buffer(device_, vertices.data(), vertices.size() * sizeof(RmlMetalVertex), @"RmlUi transient vertices");
        id<MTLBuffer> index_buffer = create_buffer(device_, indices.data(), indices.size() * sizeof(std::uint32_t), @"RmlUi transient indices");
        if (vertex_buffer == nil || index_buffer == nil)
        {
            return;
        }

        const RmlMetalTexture* texture_data = reinterpret_cast<const RmlMetalTexture*>(texture);
        id<MTLTexture> texture_resource = texture_data != nullptr && texture_data->texture != nil ? texture_data->texture : white_texture_->texture;
        const std::array<float, 2> viewport{static_cast<float>(width_), static_cast<float>(height_)};

        [encoder_ setRenderPipelineState:pipeline_];
        [encoder_ setDepthStencilState:nil];
        [encoder_ setCullMode:MTLCullModeNone];
        [encoder_ setVertexBuffer:vertex_buffer offset:0 atIndex:0];
        [encoder_ setVertexBytes:viewport.data() length:viewport.size() * sizeof(float) atIndex:1];
        [encoder_ setFragmentTexture:texture_resource atIndex:0];
        [encoder_ setFragmentSamplerState:sampler_ atIndex:0];
        [encoder_ setScissorRect:current_scissor()];
        [encoder_ drawIndexedPrimitives:MTLPrimitiveTypeTriangle
                              indexCount:indices.size()
                               indexType:MTLIndexTypeUInt32
                             indexBuffer:index_buffer
                       indexBufferOffset:0];
    }

    void ReleaseGeometry(Rml::CompiledGeometryHandle handle) override
    {
        auto* geometry = reinterpret_cast<RmlMetalGeometry*>(handle);
        geometries_.erase(std::remove_if(geometries_.begin(), geometries_.end(), [geometry](const std::unique_ptr<RmlMetalGeometry>& entry) {
            return entry.get() == geometry;
        }), geometries_.end());
    }

    Rml::TextureHandle LoadTexture(Rml::Vector2i& texture_dimensions, const Rml::String& source) override
    {
        texture_dimensions = {};
        const std::string normalized_source = normalize_texture_source(source);
        RmlMetalTexture* texture = is_video_texture_source(normalized_source)
            ? create_video_placeholder(normalized_source, texture_dimensions)
            : load_texture_file(normalized_source, texture_dimensions);
        return reinterpret_cast<Rml::TextureHandle>(texture);
    }

    Rml::TextureHandle GenerateTexture(Rml::Span<const Rml::byte> source, Rml::Vector2i source_dimensions) override
    {
        if (source_dimensions.x <= 0 || source_dimensions.y <= 0 || source.empty())
        {
            return {};
        }

        std::vector<Rml::byte> pixels(source.begin(), source.end());
        premultiply_alpha(pixels);
        RmlMetalTexture* texture = create_texture(pixels.data(),
            static_cast<std::uint32_t>(source_dimensions.x),
            static_cast<std::uint32_t>(source_dimensions.y),
            "RmlUi generated");
        return reinterpret_cast<Rml::TextureHandle>(texture);
    }

    void ReleaseTexture(Rml::TextureHandle) override
    {
        // Textures are retained until renderer shutdown so in-flight frames remain valid.
    }

    void EnableScissorRegion(bool enable) override
    {
        scissor_enabled_ = enable;
    }

    void SetScissorRegion(Rml::Rectanglei region) override
    {
        scissor_region_ = region;
    }

private:
    MTLScissorRect current_scissor() const
    {
        if (!scissor_enabled_)
        {
            return MTLScissorRect{0, 0, static_cast<NSUInteger>(width_), static_cast<NSUInteger>(height_)};
        }

        const int left = std::clamp(scissor_region_.Left(), 0, static_cast<int>(width_));
        const int top = std::clamp(scissor_region_.Top(), 0, static_cast<int>(height_));
        const int right = std::clamp(scissor_region_.Right(), left, static_cast<int>(width_));
        const int bottom = std::clamp(scissor_region_.Bottom(), top, static_cast<int>(height_));
        return MTLScissorRect{
            static_cast<NSUInteger>(left),
            static_cast<NSUInteger>(top),
            static_cast<NSUInteger>(std::max(0, right - left)),
            static_cast<NSUInteger>(std::max(0, bottom - top)),
        };
    }

    RmlMetalTexture* create_video_placeholder(const std::string& source, Rml::Vector2i& texture_dimensions)
    {
        constexpr std::uint32_t placeholder_width = 180;
        constexpr std::uint32_t placeholder_height = 52;
        std::vector<Rml::byte> pixels = make_go_button_placeholder(placeholder_width, placeholder_height);
        RmlMetalTexture* texture = create_texture(pixels.data(), placeholder_width, placeholder_height, "RmlUi Source video placeholder");
        if (texture != nullptr)
        {
            texture_dimensions = texture->dimensions;
            log_info("using static RmlUi Metal texture for Source video token '{}'", source);
        }
        return texture;
    }

    RmlMetalTexture* load_texture_file(const std::string& source, Rml::Vector2i& texture_dimensions)
    {
        std::error_code error;
        if (!std::filesystem::is_regular_file(source, error))
        {
            log_warning("RmlUi Metal texture file does not exist: '{}'", source);
            return nullptr;
        }

        NSURL* url = [NSURL fileURLWithPath:[NSString stringWithUTF8String:source.c_str()]];
        CGImageSourceRef image_source = CGImageSourceCreateWithURL((__bridge CFURLRef)url, nullptr);
        if (image_source == nullptr)
        {
            log_warning("failed to create ImageIO source for RmlUi texture '{}'", source);
            return nullptr;
        }

        CGImageRef image = CGImageSourceCreateImageAtIndex(image_source, 0, nullptr);
        CFRelease(image_source);
        if (image == nullptr)
        {
            log_warning("failed to decode RmlUi texture '{}'", source);
            return nullptr;
        }

        const std::uint32_t texture_width = static_cast<std::uint32_t>(CGImageGetWidth(image));
        const std::uint32_t texture_height = static_cast<std::uint32_t>(CGImageGetHeight(image));
        std::size_t byte_size = 0;
        if (!checked_texture_byte_size(texture_width, texture_height, byte_size))
        {
            CGImageRelease(image);
            log_warning("RmlUi texture '{}' has invalid dimensions {}x{}", source, texture_width, texture_height);
            return nullptr;
        }

        std::vector<Rml::byte> pixels(byte_size);
        CGColorSpaceRef color_space = CGColorSpaceCreateDeviceRGB();
        CGContextRef context = CGBitmapContextCreate(pixels.data(),
            texture_width,
            texture_height,
            8,
            static_cast<std::size_t>(texture_width) * 4U,
            color_space,
            static_cast<std::uint32_t>(kCGImageAlphaPremultipliedLast) | static_cast<std::uint32_t>(kCGBitmapByteOrder32Big));
        CGColorSpaceRelease(color_space);
        if (context == nullptr)
        {
            CGImageRelease(image);
            return nullptr;
        }

        CGContextTranslateCTM(context, 0.0, static_cast<CGFloat>(texture_height));
        CGContextScaleCTM(context, 1.0, -1.0);
        CGContextDrawImage(context, CGRectMake(0.0, 0.0, static_cast<CGFloat>(texture_width), static_cast<CGFloat>(texture_height)), image);
        CGContextRelease(context);
        CGImageRelease(image);

        RmlMetalTexture* texture = create_texture(pixels.data(), texture_width, texture_height, source.c_str());
        if (texture != nullptr)
        {
            texture_dimensions = texture->dimensions;
        }
        return texture;
    }

    RmlMetalTexture* create_texture(const Rml::byte* pixels, std::uint32_t texture_width, std::uint32_t texture_height, const char* debug_name)
    {
        if (device_ == nil || pixels == nullptr || texture_width == 0 || texture_height == 0)
        {
            return nullptr;
        }

        MTLTextureDescriptor* descriptor = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm
                                                                                               width:texture_width
                                                                                              height:texture_height
                                                                                           mipmapped:NO];
        descriptor.usage = MTLTextureUsageShaderRead;
        descriptor.storageMode = MTLStorageModeShared;
        id<MTLTexture> texture_resource = [device_ newTextureWithDescriptor:descriptor];
        if (texture_resource == nil)
        {
            return nullptr;
        }
        if (debug_name != nullptr)
        {
            texture_resource.label = [NSString stringWithUTF8String:debug_name];
        }

        MTLRegion region = MTLRegionMake2D(0, 0, texture_width, texture_height);
        [texture_resource replaceRegion:region mipmapLevel:0 withBytes:pixels bytesPerRow:static_cast<NSUInteger>(texture_width) * 4U];

        auto texture = std::make_unique<RmlMetalTexture>();
        texture->texture = texture_resource;
        texture->dimensions = Rml::Vector2i(static_cast<int>(texture_width), static_cast<int>(texture_height));
        RmlMetalTexture* result = texture.get();
        textures_.push_back(std::move(texture));
        return result;
    }

    id<MTLDevice> device_ = nil;
    id<MTLLibrary> library_ = nil;
    id<MTLRenderPipelineState> pipeline_ = nil;
    id<MTLSamplerState> sampler_ = nil;
    id<MTLRenderCommandEncoder> encoder_ = nil;
    RmlMetalTexture* white_texture_ = nullptr;
    std::vector<std::unique_ptr<RmlMetalGeometry>> geometries_;
    std::vector<std::unique_ptr<RmlMetalTexture>> textures_;
    std::uint32_t width_ = 1;
    std::uint32_t height_ = 1;
    bool scissor_enabled_ = false;
    Rml::Rectanglei scissor_region_ = Rml::Rectanglei::MakeInvalid();
};

}

struct MetalRenderer::Impl
{
    EngineContext* engine_context = nullptr;
    SDL_Window* window = nullptr;
    SDL_MetalView metal_view = nullptr;
    CAMetalLayer* layer = nil;
    id<MTLDevice> device = nil;
    id<MTLCommandQueue> command_queue = nil;
    id<MTLLibrary> shader_library = nil;
    id<MTLRenderPipelineState> world_pipeline = nil;
    id<MTLRenderPipelineState> skybox_pipeline = nil;
    id<MTLDepthStencilState> world_depth_state = nil;
    id<MTLDepthStencilState> skybox_depth_state = nil;
    id<MTLSamplerState> world_sampler = nil;
    id<MTLSamplerState> lightmap_sampler = nil;
    id<MTLSamplerState> skybox_sampler = nil;
    id<MTLTexture> depth_texture = nil;
    id<MTLCommandBuffer> in_flight_command_buffer = nil;
    std::unique_ptr<WorldGpuResources> world_gpu;
    std::unique_ptr<SkyboxGpuResources> skybox_gpu;
    std::unique_ptr<RmlMetalRenderInterface> rml_render_interface;
    std::unique_ptr<RmlUiLayer> rml_ui_layer;
    MetalFrameLights frame_lights;
    std::uint32_t width = 1280;
    std::uint32_t height = 720;
    bool initialized = false;
    bool sdl_initialized = false;
    bool window_closed = false;
    bool vsync = true;
    bool mouse_captured = false;

    bool initialize(const RuntimeConfig& config)
    {
        if (initialized)
        {
            return true;
        }

        width = config.window_width;
        height = config.window_height;
        vsync = config.vsync;

        SDL_SetMainReady();
        if (!SDL_Init(SDL_INIT_VIDEO))
        {
            log_error("SDL_Init failed: {}", SDL_GetError());
            return false;
        }
        sdl_initialized = true;

        const SDL_WindowFlags flags = SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY | SDL_WINDOW_METAL;
        window = SDL_CreateWindow(kWindowTitle, static_cast<int>(width), static_cast<int>(height), flags);
        if (window == nullptr)
        {
            log_error("SDL_CreateWindow failed: {}", SDL_GetError());
            return false;
        }

        int pixel_width = 0;
        int pixel_height = 0;
        if (SDL_GetWindowSizeInPixels(window, &pixel_width, &pixel_height) && pixel_width > 0 && pixel_height > 0)
        {
            width = static_cast<std::uint32_t>(pixel_width);
            height = static_cast<std::uint32_t>(pixel_height);
        }

        device = MTLCreateSystemDefaultDevice();
        if (device == nil)
        {
            log_error("Metal is not available on this system");
            return false;
        }

        metal_view = SDL_Metal_CreateView(window);
        if (metal_view == nullptr)
        {
            log_error("SDL_Metal_CreateView failed: {}", SDL_GetError());
            return false;
        }

        layer = (__bridge CAMetalLayer*)SDL_Metal_GetLayer(metal_view);
        if (layer == nil)
        {
            log_error("SDL_Metal_GetLayer did not return a CAMetalLayer");
            return false;
        }

        layer.device = device;
        layer.pixelFormat = kRenderTargetFormat;
        layer.framebufferOnly = YES;
        layer.drawableSize = CGSizeMake(width, height);
        if ([layer respondsToSelector:@selector(setDisplaySyncEnabled:)])
        {
            layer.displaySyncEnabled = vsync ? YES : NO;
        }

        command_queue = [device newCommandQueue];
        if (command_queue == nil)
        {
            log_error("failed to create Metal command queue");
            return false;
        }

        if (!create_pipeline_objects() || !create_depth_texture(width, height) || !initialize_rml(config))
        {
            return false;
        }

        SDL_ShowWindow(window);
        initialized = true;
        log_info("metal renderer initialized {}x{} vsync={} device={}", width, height, vsync ? "on" : "off", [[device name] UTF8String]);
        return true;
    }

    bool create_pipeline_objects()
    {
        NSError* error = nil;
        shader_library = [device newLibraryWithSource:[NSString stringWithUTF8String:metal_shader_source()] options:nil error:&error];
        if (shader_library == nil)
        {
            log_error("Metal shader compile failed: {}", error != nil ? [[error localizedDescription] UTF8String] : "unknown error");
            return false;
        }

        id<MTLFunction> world_vertex = [shader_library newFunctionWithName:@"world_vertex_main"];
        id<MTLFunction> world_fragment = [shader_library newFunctionWithName:@"world_fragment_main"];
        id<MTLFunction> skybox_vertex = [shader_library newFunctionWithName:@"skybox_vertex_main"];
        id<MTLFunction> skybox_fragment = [shader_library newFunctionWithName:@"skybox_fragment_main"];
        if (world_vertex == nil || world_fragment == nil || skybox_vertex == nil || skybox_fragment == nil)
        {
            log_error("Metal shader library is missing one or more renderer entry points");
            return false;
        }

        MTLRenderPipelineDescriptor* world_descriptor = [[MTLRenderPipelineDescriptor alloc] init];
        world_descriptor.label = @"OpenStrike World Pipeline";
        world_descriptor.vertexFunction = world_vertex;
        world_descriptor.fragmentFunction = world_fragment;
        world_descriptor.colorAttachments[0].pixelFormat = kRenderTargetFormat;
        world_descriptor.depthAttachmentPixelFormat = kDepthFormat;
        world_pipeline = [device newRenderPipelineStateWithDescriptor:world_descriptor error:&error];
        if (world_pipeline == nil)
        {
            log_error("failed to create Metal world pipeline: {}", error != nil ? [[error localizedDescription] UTF8String] : "unknown error");
            return false;
        }

        MTLRenderPipelineDescriptor* skybox_descriptor = [[MTLRenderPipelineDescriptor alloc] init];
        skybox_descriptor.label = @"OpenStrike Skybox Pipeline";
        skybox_descriptor.vertexFunction = skybox_vertex;
        skybox_descriptor.fragmentFunction = skybox_fragment;
        skybox_descriptor.colorAttachments[0].pixelFormat = kRenderTargetFormat;
        skybox_descriptor.depthAttachmentPixelFormat = kDepthFormat;
        skybox_pipeline = [device newRenderPipelineStateWithDescriptor:skybox_descriptor error:&error];
        if (skybox_pipeline == nil)
        {
            log_error("failed to create Metal skybox pipeline: {}", error != nil ? [[error localizedDescription] UTF8String] : "unknown error");
            return false;
        }

        MTLDepthStencilDescriptor* world_depth = [[MTLDepthStencilDescriptor alloc] init];
        world_depth.depthCompareFunction = MTLCompareFunctionLessEqual;
        world_depth.depthWriteEnabled = YES;
        world_depth_state = [device newDepthStencilStateWithDescriptor:world_depth];

        MTLDepthStencilDescriptor* skybox_depth = [[MTLDepthStencilDescriptor alloc] init];
        skybox_depth.depthCompareFunction = MTLCompareFunctionLessEqual;
        skybox_depth.depthWriteEnabled = NO;
        skybox_depth_state = [device newDepthStencilStateWithDescriptor:skybox_depth];

        MTLStencilDescriptor* unused = nil;
        (void)unused;

        MTLTextureUsage usage = MTLTextureUsageShaderRead;
        (void)usage;

        MTLSamplerDescriptor* world_sampler_descriptor = [[MTLSamplerDescriptor alloc] init];
        world_sampler_descriptor.minFilter = MTLSamplerMinMagFilterLinear;
        world_sampler_descriptor.magFilter = MTLSamplerMinMagFilterLinear;
        world_sampler_descriptor.mipFilter = MTLSamplerMipFilterLinear;
        world_sampler_descriptor.sAddressMode = MTLSamplerAddressModeRepeat;
        world_sampler_descriptor.tAddressMode = MTLSamplerAddressModeRepeat;
        world_sampler = [device newSamplerStateWithDescriptor:world_sampler_descriptor];

        MTLSamplerDescriptor* clamp_sampler_descriptor = [[MTLSamplerDescriptor alloc] init];
        clamp_sampler_descriptor.minFilter = MTLSamplerMinMagFilterLinear;
        clamp_sampler_descriptor.magFilter = MTLSamplerMinMagFilterLinear;
        clamp_sampler_descriptor.mipFilter = MTLSamplerMipFilterLinear;
        clamp_sampler_descriptor.sAddressMode = MTLSamplerAddressModeClampToEdge;
        clamp_sampler_descriptor.tAddressMode = MTLSamplerAddressModeClampToEdge;
        lightmap_sampler = [device newSamplerStateWithDescriptor:clamp_sampler_descriptor];
        skybox_sampler = [device newSamplerStateWithDescriptor:clamp_sampler_descriptor];

        if (world_depth_state == nil || skybox_depth_state == nil || world_sampler == nil || lightmap_sampler == nil || skybox_sampler == nil)
        {
            log_error("failed to create one or more Metal render states");
            return false;
        }

        return true;
    }

    bool create_depth_texture(std::uint32_t target_width, std::uint32_t target_height)
    {
        if (device == nil || target_width == 0 || target_height == 0)
        {
            return false;
        }

        MTLTextureDescriptor* descriptor = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:kDepthFormat
                                                                                               width:target_width
                                                                                              height:target_height
                                                                                           mipmapped:NO];
        descriptor.usage = MTLTextureUsageRenderTarget;
        descriptor.storageMode = MTLStorageModePrivate;
        depth_texture = [device newTextureWithDescriptor:descriptor];
        if (depth_texture == nil)
        {
            log_error("failed to create Metal depth texture {}x{}", target_width, target_height);
            return false;
        }
        depth_texture.label = @"OpenStrike Depth";
        return true;
    }

    bool initialize_rml(const RuntimeConfig& config)
    {
        rml_render_interface = std::make_unique<RmlMetalRenderInterface>();
        if (!rml_render_interface->initialize(device))
        {
            return false;
        }

        rml_ui_layer = std::make_unique<RmlUiLayer>();
        return rml_ui_layer->initialize(*window, *rml_render_interface, engine_context, config, width, height);
    }

    void shutdown_rml()
    {
        rml_ui_layer.reset();
        if (rml_render_interface != nullptr)
        {
            rml_render_interface->shutdown();
        }
        rml_render_interface.reset();
    }

    void render(const FrameContext& context)
    {
        if (!initialized || window_closed)
        {
            return;
        }

        pump_messages();
        if (window_closed || width == 0 || height == 0)
        {
            return;
        }

        if (depth_texture == nil || depth_texture.width != width || depth_texture.height != height)
        {
            if (!create_depth_texture(width, height))
            {
                window_closed = true;
                return;
            }
        }

        if (rml_ui_layer != nullptr)
        {
            rml_ui_layer->update(width, height);
        }

        const LoadedWorld* active_world = engine_context != nullptr ? engine_context->world.current_world() : nullptr;
        const bool skybox_ready = active_world != nullptr && ensure_skybox_gpu_resources(*active_world);
        const bool world_ready = ensure_world_gpu_resources();
        update_light_buffer(active_world);

        @autoreleasepool
        {
            layer.drawableSize = CGSizeMake(width, height);
            id<CAMetalDrawable> drawable = [layer nextDrawable];
            if (drawable == nil)
            {
                return;
            }

            MTLRenderPassDescriptor* pass_descriptor = [MTLRenderPassDescriptor renderPassDescriptor];
            pass_descriptor.colorAttachments[0].texture = drawable.texture;
            pass_descriptor.colorAttachments[0].loadAction = MTLLoadActionClear;
            pass_descriptor.colorAttachments[0].storeAction = MTLStoreActionStore;
            pass_descriptor.colorAttachments[0].clearColor = frame_clear_color(context.frame_index);
            pass_descriptor.depthAttachment.texture = depth_texture;
            pass_descriptor.depthAttachment.loadAction = MTLLoadActionClear;
            pass_descriptor.depthAttachment.storeAction = MTLStoreActionDontCare;
            pass_descriptor.depthAttachment.clearDepth = 1.0;

            id<MTLCommandBuffer> command_buffer = [command_queue commandBuffer];
            id<MTLRenderCommandEncoder> encoder = [command_buffer renderCommandEncoderWithDescriptor:pass_descriptor];
            [encoder setViewport:MTLViewport{0.0, 0.0, static_cast<double>(width), static_cast<double>(height), 0.0, 1.0}];
            [encoder setScissorRect:MTLScissorRect{0, 0, static_cast<NSUInteger>(width), static_cast<NSUInteger>(height)}];

            if (skybox_ready)
            {
                record_skybox(encoder);
            }
            if (world_ready)
            {
                record_world(encoder);
            }
            if (rml_ui_layer != nullptr && rml_render_interface != nullptr)
            {
                rml_render_interface->begin_frame(encoder, width, height);
                rml_ui_layer->render();
                rml_render_interface->end_frame();
            }

            [encoder endEncoding];
            [command_buffer presentDrawable:drawable];
            [command_buffer commit];
            in_flight_command_buffer = command_buffer;
        }
    }

    bool ensure_world_gpu_resources()
    {
        if (engine_context == nullptr || device == nil || world_pipeline == nil)
        {
            return false;
        }

        const LoadedWorld* world = engine_context->world.current_world();
        const std::uint64_t generation = engine_context->world.generation();
        if (world == nullptr || world->mesh.vertices.empty() || world->mesh.indices.empty())
        {
            world_gpu.reset();
            return false;
        }

        if (world_gpu != nullptr && world_gpu->generation == generation)
        {
            return true;
        }

        std::vector<WorldDrawVertex> vertices;
        vertices.reserve(world->mesh.vertices.size());
        for (const WorldMeshVertex& vertex : world->mesh.vertices)
        {
            vertices.push_back(WorldDrawVertex{
                {vertex.position.x, vertex.position.y, vertex.position.z},
                {vertex.normal.x, vertex.normal.y, vertex.normal.z},
                {vertex.texcoord.x, vertex.texcoord.y},
                {vertex.lightmap_texcoord.x, vertex.lightmap_texcoord.y},
                vertex.lightmap_weight,
            });
        }

        auto gpu = std::make_unique<WorldGpuResources>();
        gpu->vertex_buffer = create_buffer(device,
            vertices.data(),
            vertices.size() * sizeof(WorldDrawVertex),
            [NSString stringWithUTF8String:std::format("OpenStrike World {} Vertex Buffer", world->name).c_str()]);
        gpu->index_buffer = create_buffer(device,
            world->mesh.indices.data(),
            world->mesh.indices.size() * sizeof(std::uint32_t),
            [NSString stringWithUTF8String:std::format("OpenStrike World {} Index Buffer", world->name).c_str()]);
        if (gpu->vertex_buffer == nil || gpu->index_buffer == nil)
        {
            log_warning("failed to create Metal world buffers for '{}'", world->name);
            return false;
        }
        gpu->index_count = static_cast<std::uint32_t>(world->mesh.indices.size());

        SourceAssetStore source_assets(engine_context->filesystem, &world->embedded_assets);
        MaterialSystem material_system(source_assets);
        const std::uint32_t material_count = std::max<std::uint32_t>(static_cast<std::uint32_t>(world->mesh.materials.size()), 1U);
        gpu->textures.reserve(material_count);
        gpu->material_constants.reserve(material_count);
        for (std::uint32_t material_index = 0; material_index < material_count; ++material_index)
        {
            const WorldMaterial fallback_material{.name = "__missing", .width = 1, .height = 1};
            const WorldMaterial& material = material_index < world->mesh.materials.size() ? world->mesh.materials[material_index] : fallback_material;
            LoadedMaterial loaded_material = material_system.load_world_material(material.name);
            id<MTLTexture> texture = upload_source_texture_to_metal(device,
                loaded_material.base_texture,
                [NSString stringWithUTF8String:std::format("OpenStrike Material {}", material.name).c_str()]);
            if (texture == nil)
            {
                SourceTexture fallback = material_system.fallback_texture(material.name);
                texture = upload_source_texture_to_metal(device,
                    fallback,
                    [NSString stringWithUTF8String:std::format("OpenStrike Fallback Material {}", material.name).c_str()]);
                loaded_material.base_texture_loaded = false;
                loaded_material.using_fallback_texture = true;
                loaded_material.definition.constants.flags |= MaterialFlags::FallbackTexture;
            }
            if (texture == nil)
            {
                log_warning("failed to upload Metal material '{}'", material.name);
                return false;
            }

            if (loaded_material.base_texture_loaded)
            {
                ++gpu->loaded_texture_count;
            }
            else
            {
                ++gpu->fallback_texture_count;
            }
            gpu->textures.push_back(texture);
            gpu->material_constants.push_back(loaded_material.definition.constants);
        }

        gpu->lightmap_texture = upload_lightmap_to_metal(device,
            world->mesh.lightmap_atlas,
            [NSString stringWithUTF8String:std::format("OpenStrike World {} Lightmap Atlas", world->name).c_str()]);
        if (gpu->lightmap_texture == nil)
        {
            log_warning("failed to upload Metal world lightmap for '{}'", world->name);
            return false;
        }

        gpu->batches.reserve(world->mesh.batches.empty() ? 1 : world->mesh.batches.size());
        if (world->mesh.batches.empty())
        {
            gpu->batches.push_back(WorldDrawBatch{0, 0, gpu->index_count});
        }
        else
        {
            for (const WorldMeshBatch& batch : world->mesh.batches)
            {
                gpu->batches.push_back(WorldDrawBatch{
                    .material_index = batch.material_index < material_count ? batch.material_index : 0U,
                    .first_index = batch.first_index,
                    .index_count = batch.index_count,
                });
            }
        }

        gpu->generation = generation;
        world_gpu = std::move(gpu);
        log_info("uploaded Metal world mesh '{}' vertices={} triangles={} materials={} textures={} fallback_textures={} batches={}",
            world->name,
            world->mesh.vertices.size(),
            world->mesh.indices.size() / 3,
            world->mesh.materials.size(),
            world_gpu->loaded_texture_count,
            world_gpu->fallback_texture_count,
            world_gpu->batches.size());
        return true;
    }

    bool ensure_skybox_gpu_resources(const LoadedWorld& world)
    {
        if (engine_context == nullptr || device == nil || skybox_pipeline == nil)
        {
            return false;
        }

        const std::uint64_t generation = engine_context->world.generation();
        if (world.kind != WorldAssetKind::SourceBsp || !world.mesh.has_sky_surfaces)
        {
            skybox_gpu.reset();
            return false;
        }

        const std::optional<std::string> requested_sky_name = skybox_name_from_world(world);
        if (!requested_sky_name || requested_sky_name->empty())
        {
            skybox_gpu.reset();
            return false;
        }

        if (skybox_gpu != nullptr && skybox_gpu->generation == generation && skybox_gpu->sky_name == *requested_sky_name)
        {
            return true;
        }

        static constexpr std::array<std::string_view, kSkyboxFaceCount> kSkyboxSuffixByDrawAxis{"rt", "lf", "bk", "ft", "up", "dn"};
        SourceAssetStore source_assets(engine_context->filesystem, &world.embedded_assets);
        MaterialSystem material_system(source_assets);

        auto load_skybox_textures = [&](std::string_view sky_name) -> std::optional<std::array<SourceTexture, kSkyboxFaceCount>> {
            std::array<SourceTexture, kSkyboxFaceCount> textures{};
            for (std::size_t face = 0; face < kSkyboxFaceCount; ++face)
            {
                std::string material_name = "skybox/" + std::string(sky_name) + std::string(kSkyboxSuffixByDrawAxis[face]);
                LoadedMaterial loaded = material_system.load_world_material(material_name);
                if (!loaded.base_texture_loaded)
                {
                    return std::nullopt;
                }
                textures[face] = std::move(loaded.base_texture);
            }
            return textures;
        };

        std::string resolved_sky_name = *requested_sky_name;
        std::optional<std::array<SourceTexture, kSkyboxFaceCount>> sky_textures = load_skybox_textures(resolved_sky_name);
        if (!sky_textures && resolved_sky_name != "sky_urb01")
        {
            log_warning("skybox '{}' is incomplete for Metal; trying default sky_urb01", resolved_sky_name);
            resolved_sky_name = "sky_urb01";
            sky_textures = load_skybox_textures(resolved_sky_name);
        }

        if (!sky_textures)
        {
            log_warning("skybox '{}' is missing one or more Source VTF faces; Metal sky draw disabled", resolved_sky_name);
            skybox_gpu.reset();
            return false;
        }

        auto gpu = std::make_unique<SkyboxGpuResources>();
        gpu->generation = generation;
        gpu->sky_name = *requested_sky_name;

        const std::array<SkyboxDrawVertex, kSkyboxFaceCount * 4> vertices = build_skybox_vertices();
        const std::array<std::uint16_t, kSkyboxFaceCount * 6> indices = build_skybox_indices();
        gpu->vertex_buffer = create_buffer(device, vertices.data(), vertices.size() * sizeof(SkyboxDrawVertex), @"OpenStrike Skybox Vertex Buffer");
        gpu->index_buffer = create_buffer(device, indices.data(), indices.size() * sizeof(std::uint16_t), @"OpenStrike Skybox Index Buffer");
        if (gpu->vertex_buffer == nil || gpu->index_buffer == nil)
        {
            return false;
        }

        for (std::size_t face = 0; face < kSkyboxFaceCount; ++face)
        {
            gpu->textures[face] = upload_source_texture_to_metal(device,
                (*sky_textures)[face],
                [NSString stringWithUTF8String:std::format("OpenStrike Skybox {} {}", resolved_sky_name, kSkyboxSuffixByDrawAxis[face]).c_str()]);
            if (gpu->textures[face] == nil)
            {
                log_warning("failed to upload Metal skybox '{}' face '{}'", resolved_sky_name, kSkyboxSuffixByDrawAxis[face]);
                return false;
            }
        }

        log_info("uploaded Metal skybox '{}' from Source faces", resolved_sky_name);
        skybox_gpu = std::move(gpu);
        return true;
    }

    void update_light_buffer(const LoadedWorld* world)
    {
        std::array<MetalLightGpu, kMetalMaxLights> lights{};
        std::uint32_t count = 0;
        if (world != nullptr)
        {
            count = std::min<std::uint32_t>(static_cast<std::uint32_t>(world->lights.size()), kMetalMaxLights);
            for (std::uint32_t index = 0; index < count; ++index)
            {
                lights[index] = metal_light_gpu(world->lights[index]);
            }
        }

        frame_lights.buffer = create_buffer(device, lights.data(), lights.size() * sizeof(MetalLightGpu), @"OpenStrike Metal Lights");
        frame_lights.light_count = count;
    }

    void record_skybox(id<MTLRenderCommandEncoder> encoder)
    {
        if (encoder == nil || skybox_gpu == nullptr)
        {
            return;
        }

        const std::array<float, 16> constants =
            skybox_to_clip_matrix(engine_context != nullptr ? &engine_context->camera : nullptr, width, height);
        [encoder setRenderPipelineState:skybox_pipeline];
        [encoder setDepthStencilState:skybox_depth_state];
        [encoder setCullMode:MTLCullModeNone];
        [encoder setVertexBuffer:skybox_gpu->vertex_buffer offset:0 atIndex:0];
        [encoder setVertexBytes:constants.data() length:constants.size() * sizeof(float) atIndex:1];
        [encoder setFragmentSamplerState:skybox_sampler atIndex:0];
        for (std::uint32_t face = 0; face < kSkyboxFaceCount; ++face)
        {
            [encoder setFragmentTexture:skybox_gpu->textures[face] atIndex:0];
            [encoder drawIndexedPrimitives:MTLPrimitiveTypeTriangle
                                indexCount:6
                                 indexType:MTLIndexTypeUInt16
                               indexBuffer:skybox_gpu->index_buffer
                         indexBufferOffset:static_cast<NSUInteger>(face * 6U * sizeof(std::uint16_t))];
        }
    }

    void record_world(id<MTLRenderCommandEncoder> encoder)
    {
        if (encoder == nil || world_gpu == nullptr || engine_context == nullptr || frame_lights.buffer == nil)
        {
            return;
        }

        const LoadedWorld* world = engine_context->world.current_world();
        if (world == nullptr)
        {
            return;
        }

        const std::array<float, kWorldShaderFloatCount> constants =
            world_shader_constants(world->mesh, &engine_context->camera, width, height, frame_lights.light_count);
        [encoder setRenderPipelineState:world_pipeline];
        [encoder setDepthStencilState:world_depth_state];
        [encoder setCullMode:MTLCullModeNone];
        [encoder setVertexBuffer:world_gpu->vertex_buffer offset:0 atIndex:0];
        [encoder setVertexBytes:constants.data() length:constants.size() * sizeof(float) atIndex:1];
        [encoder setFragmentBytes:constants.data() length:constants.size() * sizeof(float) atIndex:1];
        [encoder setFragmentBuffer:frame_lights.buffer offset:0 atIndex:2];
        [encoder setFragmentTexture:world_gpu->lightmap_texture atIndex:1];
        [encoder setFragmentSamplerState:world_sampler atIndex:0];
        [encoder setFragmentSamplerState:lightmap_sampler atIndex:1];

        std::uint32_t last_material_index = std::numeric_limits<std::uint32_t>::max();
        for (const WorldDrawBatch& batch : world_gpu->batches)
        {
            if (batch.index_count == 0)
            {
                continue;
            }

            const std::uint32_t material_index =
                batch.material_index < world_gpu->textures.size() ? batch.material_index : 0U;
            if (material_index != last_material_index)
            {
                [encoder setFragmentTexture:world_gpu->textures[material_index] atIndex:0];
                [encoder setFragmentBytes:&world_gpu->material_constants[material_index] length:sizeof(MaterialGpuConstants) atIndex:0];
                last_material_index = material_index;
            }

            [encoder drawIndexedPrimitives:MTLPrimitiveTypeTriangle
                                indexCount:batch.index_count
                                 indexType:MTLIndexTypeUInt32
                               indexBuffer:world_gpu->index_buffer
                         indexBufferOffset:static_cast<NSUInteger>(batch.first_index) * sizeof(std::uint32_t)];
        }
    }

    void shutdown()
    {
        if (!initialized && window == nullptr && metal_view == nullptr && !sdl_initialized)
        {
            return;
        }

        if (in_flight_command_buffer != nil)
        {
            [in_flight_command_buffer waitUntilCompleted];
            in_flight_command_buffer = nil;
        }

        shutdown_rml();
        world_gpu.reset();
        skybox_gpu.reset();
        frame_lights = {};
        depth_texture = nil;
        world_pipeline = nil;
        skybox_pipeline = nil;
        world_depth_state = nil;
        skybox_depth_state = nil;
        world_sampler = nil;
        lightmap_sampler = nil;
        skybox_sampler = nil;
        shader_library = nil;
        command_queue = nil;
        layer = nil;
        device = nil;

        if (metal_view != nullptr)
        {
            SDL_Metal_DestroyView(metal_view);
            metal_view = nullptr;
        }

        if (window != nullptr)
        {
            SDL_DestroyWindow(window);
            window = nullptr;
        }

        if (sdl_initialized)
        {
            SDL_QuitSubSystem(SDL_INIT_VIDEO);
            SDL_Quit();
            sdl_initialized = false;
        }

        initialized = false;
        window_closed = true;
        log_info("metal renderer shutdown");
    }

    void pump_messages()
    {
        SDL_Event event{};
        while (SDL_PollEvent(&event))
        {
            if (event.type == SDL_EVENT_QUIT)
            {
                window_closed = true;
                return;
            }

            if (event.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED && event.window.windowID == SDL_GetWindowID(window))
            {
                window_closed = true;
                return;
            }

            if (event.type == SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED && event.window.windowID == SDL_GetWindowID(window))
            {
                if (event.window.data1 > 0 && event.window.data2 > 0)
                {
                    width = static_cast<std::uint32_t>(event.window.data1);
                    height = static_cast<std::uint32_t>(event.window.data2);
                }
            }

            if (rml_ui_layer != nullptr && rml_ui_layer->handle_hotkey_event(event))
            {
                continue;
            }

            const bool ui_visible = rml_ui_layer != nullptr && rml_ui_layer->gameplay_ui_visible();
            if (engine_context != nullptr && !ui_visible)
            {
                if (handle_sdl_gameplay_input_event(engine_context->input, event))
                {
                    continue;
                }
            }

            if (rml_ui_layer != nullptr)
            {
                rml_ui_layer->dispatch_sdl_event(*window, event);
            }
        }

        if (engine_context != nullptr)
        {
            const bool should_capture = rml_ui_layer != nullptr ? rml_ui_layer->wants_mouse_capture()
                                                                : engine_context->world.current_world() != nullptr;
            if (!should_capture)
            {
                engine_context->input.clear_gameplay_buttons();
            }

            if (engine_context->input.mouse_captured != should_capture)
            {
                if (!SDL_SetWindowRelativeMouseMode(window, should_capture))
                {
                    log_warning("failed to set relative mouse mode: {}", SDL_GetError());
                }
                mouse_captured = should_capture;
                engine_context->input.mouse_captured = should_capture;
                engine_context->input.mouse_delta = {};
            }
        }
    }
};

MetalRenderer::MetalRenderer()
    : impl_(std::make_unique<Impl>())
{
}

MetalRenderer::~MetalRenderer()
{
    shutdown();
}

void MetalRenderer::set_engine_context(EngineContext* context)
{
    impl_->engine_context = context;
}

bool MetalRenderer::initialize(const RuntimeConfig& config)
{
    if (!impl_->initialize(config))
    {
        shutdown();
        return false;
    }
    return true;
}

void MetalRenderer::render(const FrameContext& context)
{
    impl_->render(context);
}

bool MetalRenderer::should_close() const
{
    return impl_->window_closed || (impl_->rml_ui_layer != nullptr && impl_->rml_ui_layer->should_quit());
}

void MetalRenderer::shutdown()
{
    if (impl_)
    {
        impl_->shutdown();
    }
}
}
