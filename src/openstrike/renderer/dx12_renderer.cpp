#include "openstrike/renderer/dx12_renderer.hpp"

#include "openstrike/core/log.hpp"
#include "openstrike/engine/engine_context.hpp"
#include "openstrike/material/material_system.hpp"
#include "openstrike/renderer/rml_dx12_render_interface.hpp"
#include "openstrike/renderer/world_material_shader.hpp"
#include "openstrike/source/source_asset_store.hpp"
#include "openstrike/source/source_texture.hpp"
#include "openstrike/ui/main_menu_controller.hpp"
#include "openstrike/ui/rml_console_controller.hpp"
#include "openstrike/ui/rml_hud_controller.hpp"
#include "openstrike/world/world.hpp"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include <d3d12.h>
#include <d3dcompiler.h>
#include <dxgi1_6.h>
#include <wrl/client.h>

#include <RmlUi/Core.h>
#include <RmlUi/Debugger.h>
#include <RmlUi_Platform_SDL.h>
#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>

#include <algorithm>
#include <array>
#include <cassert>
#include <cctype>
#include <cmath>
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <format>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

using Microsoft::WRL::ComPtr;

namespace openstrike
{
namespace
{
constexpr const char* kWindowTitle = "OpenStrike - DirectX 12";
constexpr DXGI_FORMAT kRenderTargetFormat = DXGI_FORMAT_R8G8B8A8_UNORM;
constexpr DXGI_FORMAT kDepthFormat = DXGI_FORMAT_D32_FLOAT;
constexpr float kPi = 3.14159265358979323846F;
constexpr std::uint32_t kSkyboxFaceCount = 6;

struct WorldDrawVertex
{
    float position[3]{};
    float normal[3]{};
    float texcoord[2]{};
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

struct TextureGpuResources
{
    ComPtr<ID3D12Resource> resource;
    ComPtr<ID3D12Resource> upload_buffer;
};

std::string hresult_message(const char* operation, HRESULT result)
{
    return std::string(operation) + " failed with HRESULT 0x" + std::format("{:08X}", static_cast<unsigned>(result));
}

bool succeeded(HRESULT result, const char* operation)
{
    if (SUCCEEDED(result))
    {
        return true;
    }

    log_error("{}", hresult_message(operation, result));
    return false;
}

D3D12_HEAP_PROPERTIES heap_properties(D3D12_HEAP_TYPE type)
{
    D3D12_HEAP_PROPERTIES properties{};
    properties.Type = type;
    properties.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_UNKNOWN;
    properties.MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN;
    properties.CreationNodeMask = 1;
    properties.VisibleNodeMask = 1;
    return properties;
}

D3D12_RESOURCE_DESC buffer_desc(std::uint64_t size)
{
    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    desc.Alignment = 0;
    desc.Width = std::max<std::uint64_t>(size, 1);
    desc.Height = 1;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.Format = DXGI_FORMAT_UNKNOWN;
    desc.SampleDesc.Count = 1;
    desc.SampleDesc.Quality = 0;
    desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    desc.Flags = D3D12_RESOURCE_FLAG_NONE;
    return desc;
}

D3D12_RESOURCE_DESC texture2d_desc(std::uint32_t width, std::uint32_t height, std::uint16_t mip_levels, DXGI_FORMAT format)
{
    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    desc.Alignment = 0;
    desc.Width = std::max<std::uint32_t>(width, 1);
    desc.Height = std::max<std::uint32_t>(height, 1);
    desc.DepthOrArraySize = 1;
    desc.MipLevels = std::max<std::uint16_t>(mip_levels, 1);
    desc.Format = format;
    desc.SampleDesc.Count = 1;
    desc.SampleDesc.Quality = 0;
    desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    desc.Flags = D3D12_RESOURCE_FLAG_NONE;
    return desc;
}

DXGI_FORMAT dxgi_format_for_source(SourceTextureFormat format)
{
    switch (format)
    {
    case SourceTextureFormat::Bc1:
        return DXGI_FORMAT_BC1_UNORM;
    case SourceTextureFormat::Bc2:
        return DXGI_FORMAT_BC2_UNORM;
    case SourceTextureFormat::Bc3:
        return DXGI_FORMAT_BC3_UNORM;
    case SourceTextureFormat::Rgba8:
        return DXGI_FORMAT_R8G8B8A8_UNORM;
    }

    return DXGI_FORMAT_R8G8B8A8_UNORM;
}

bool upload_source_texture_to_gpu(
    ID3D12Device* device,
    ID3D12GraphicsCommandList* command_list,
    const SourceTexture& texture,
    D3D12_CPU_DESCRIPTOR_HANDLE cpu_handle,
    TextureGpuResources& output)
{
    if (device == nullptr || command_list == nullptr || texture.mips.empty())
    {
        return false;
    }

    const DXGI_FORMAT format = dxgi_format_for_source(texture.format);
    const auto mip_count = static_cast<UINT>(texture.mips.size());
    D3D12_RESOURCE_DESC desc = texture2d_desc(texture.width, texture.height, static_cast<std::uint16_t>(mip_count), format);
    const auto default_heap = heap_properties(D3D12_HEAP_TYPE_DEFAULT);
    if (!succeeded(device->CreateCommittedResource(&default_heap,
            D3D12_HEAP_FLAG_NONE,
            &desc,
            D3D12_RESOURCE_STATE_COPY_DEST,
            nullptr,
            IID_PPV_ARGS(&output.resource)),
            "ID3D12Device::CreateCommittedResource texture"))
    {
        return false;
    }

    std::vector<D3D12_PLACED_SUBRESOURCE_FOOTPRINT> layouts(mip_count);
    std::vector<UINT> row_counts(mip_count);
    std::vector<UINT64> row_sizes(mip_count);
    UINT64 upload_bytes = 0;
    device->GetCopyableFootprints(&desc, 0, mip_count, 0, layouts.data(), row_counts.data(), row_sizes.data(), &upload_bytes);

    const auto upload_heap = heap_properties(D3D12_HEAP_TYPE_UPLOAD);
    const auto upload_desc = buffer_desc(upload_bytes);
    if (!succeeded(device->CreateCommittedResource(&upload_heap,
            D3D12_HEAP_FLAG_NONE,
            &upload_desc,
            D3D12_RESOURCE_STATE_GENERIC_READ,
            nullptr,
            IID_PPV_ARGS(&output.upload_buffer)),
            "ID3D12Device::CreateCommittedResource texture upload"))
    {
        return false;
    }

    unsigned char* mapped = nullptr;
    if (!succeeded(output.upload_buffer->Map(0, nullptr, reinterpret_cast<void**>(&mapped)), "ID3D12Resource::Map texture upload"))
    {
        return false;
    }

    for (UINT mip = 0; mip < mip_count; ++mip)
    {
        const SourceTextureMip& source_mip = texture.mips[mip];
        const std::uint32_t source_row_bytes = source_texture_row_bytes(texture.format, source_mip.width);
        const std::uint32_t source_row_count = source_texture_row_count(texture.format, source_mip.height);
        const std::uint64_t required_bytes = static_cast<std::uint64_t>(source_row_bytes) * source_row_count;
        if (source_mip.bytes.size() < required_bytes)
        {
            output.upload_buffer->Unmap(0, nullptr);
            return false;
        }

        unsigned char* dst = mapped + layouts[mip].Offset;
        const unsigned char* src = source_mip.bytes.data();
        for (std::uint32_t row = 0; row < source_row_count; ++row)
        {
            std::memcpy(dst + (static_cast<std::uint64_t>(row) * layouts[mip].Footprint.RowPitch),
                src + (static_cast<std::uint64_t>(row) * source_row_bytes),
                source_row_bytes);
        }
    }
    output.upload_buffer->Unmap(0, nullptr);

    for (UINT mip = 0; mip < mip_count; ++mip)
    {
        D3D12_TEXTURE_COPY_LOCATION src{};
        src.pResource = output.upload_buffer.Get();
        src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        src.PlacedFootprint = layouts[mip];

        D3D12_TEXTURE_COPY_LOCATION dst{};
        dst.pResource = output.resource.Get();
        dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        dst.SubresourceIndex = mip;

        command_list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
    }

    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = output.resource.Get();
    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
    barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    command_list->ResourceBarrier(1, &barrier);

    D3D12_SHADER_RESOURCE_VIEW_DESC srv_desc{};
    srv_desc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srv_desc.Format = format;
    srv_desc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    srv_desc.Texture2D.MostDetailedMip = 0;
    srv_desc.Texture2D.MipLevels = mip_count;
    srv_desc.Texture2D.ResourceMinLODClamp = 0.0F;
    device->CreateShaderResourceView(output.resource.Get(), &srv_desc, cpu_handle);
    return true;
}

ComPtr<ID3DBlob> compile_shader(const char* source, const char* entry, const char* target)
{
    UINT flags = D3DCOMPILE_ENABLE_STRICTNESS;
#if defined(_DEBUG)
    flags |= D3DCOMPILE_DEBUG | D3DCOMPILE_SKIP_OPTIMIZATION;
#endif

    ComPtr<ID3DBlob> shader;
    ComPtr<ID3DBlob> errors;
    const HRESULT result = D3DCompile(source, std::strlen(source), nullptr, nullptr, nullptr, entry, target, flags, 0, &shader, &errors);
    if (FAILED(result))
    {
        if (errors)
        {
            log_error("renderer shader compile error: {}", static_cast<const char*>(errors->GetBufferPointer()));
        }
        else
        {
            log_error("{}", hresult_message("D3DCompile", result));
        }
        return {};
    }

    return shader;
}

bool query_tearing_support(IDXGIFactory5* factory)
{
    BOOL allow_tearing = FALSE;
    if (FAILED(factory->CheckFeatureSupport(DXGI_FEATURE_PRESENT_ALLOW_TEARING, &allow_tearing, sizeof(allow_tearing))))
    {
        return false;
    }

    return allow_tearing == TRUE;
}

ComPtr<IDXGIAdapter1> choose_hardware_adapter(IDXGIFactory6* factory)
{
    for (UINT adapter_index = 0;; ++adapter_index)
    {
        ComPtr<IDXGIAdapter1> adapter;
        if (factory->EnumAdapterByGpuPreference(
                adapter_index,
                DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE,
                IID_PPV_ARGS(&adapter)) == DXGI_ERROR_NOT_FOUND)
        {
            break;
        }

        DXGI_ADAPTER_DESC1 desc{};
        adapter->GetDesc1(&desc);
        if ((desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) != 0)
        {
            continue;
        }

        if (SUCCEEDED(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_11_0, __uuidof(ID3D12Device), nullptr)))
        {
            return adapter;
        }
    }

    return {};
}

std::string rml_path_string(const std::filesystem::path& path)
{
    return path.lexically_normal().generic_string();
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

bool gameplay_ui_visible(const RmlConsoleController* console, const MainMenuController* main_menu)
{
    return (console != nullptr && console->visible()) || (main_menu != nullptr && main_menu->visible());
}

bool set_gameplay_key(InputState& input, SDL_Keycode key, bool pressed)
{
    switch (key)
    {
    case SDLK_W:
        input.move_forward = pressed;
        return true;
    case SDLK_S:
        input.move_back = pressed;
        return true;
    case SDLK_A:
        input.move_left = pressed;
        return true;
    case SDLK_D:
        input.move_right = pressed;
        return true;
    case SDLK_SPACE:
        input.jump = pressed;
        return true;
    case SDLK_LSHIFT:
        input.sprint = pressed;
        return true;
    default:
        return false;
    }
}
}

struct Dx12Renderer::WorldGpuResources
{
    Microsoft::WRL::ComPtr<ID3D12Resource> vertex_buffer;
    Microsoft::WRL::ComPtr<ID3D12Resource> index_buffer;
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> srv_heap;
    D3D12_VERTEX_BUFFER_VIEW vertex_view{};
    D3D12_INDEX_BUFFER_VIEW index_view{};
    std::vector<TextureGpuResources> textures;
    std::vector<MaterialGpuConstants> material_constants;
    std::vector<WorldDrawBatch> batches;
    std::uint32_t srv_descriptor_size = 0;
    std::uint32_t index_count = 0;
    std::uint32_t loaded_texture_count = 0;
    std::uint32_t fallback_texture_count = 0;
};

struct Dx12Renderer::SkyboxGpuResources
{
    std::string sky_name;
    Microsoft::WRL::ComPtr<ID3D12Resource> vertex_buffer;
    Microsoft::WRL::ComPtr<ID3D12Resource> index_buffer;
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> srv_heap;
    D3D12_VERTEX_BUFFER_VIEW vertex_view{};
    D3D12_INDEX_BUFFER_VIEW index_view{};
    std::array<TextureGpuResources, kSkyboxFaceCount> textures;
    std::uint32_t srv_descriptor_size = 0;
};

Dx12Renderer::Dx12Renderer() = default;

Dx12Renderer::~Dx12Renderer()
{
    shutdown();
}

void Dx12Renderer::set_engine_context(EngineContext* context)
{
    engine_context_ = context;
}

bool Dx12Renderer::initialize(const RuntimeConfig& config)
{
    if (initialized_)
    {
        return true;
    }

    width_ = config.window_width;
    height_ = config.window_height;
    vsync_ = config.vsync;

    if (!create_window(config) || !create_device_objects(config) || !create_world_pipeline() || !create_skybox_pipeline() || !create_render_targets() ||
        !create_depth_buffer() || !initialize_rml(config))
    {
        shutdown();
        return false;
    }

    initialized_ = true;
    log_info("dx12 renderer initialized {}x{} vsync={}", width_, height_, vsync_ ? "on" : "off");
    return true;
}

void Dx12Renderer::render(const FrameContext& context)
{
    if (!initialized_ || window_closed_)
    {
        return;
    }

    pump_messages();
    if (window_closed_)
    {
        return;
    }

    if (resize_pending_ && width_ > 0 && height_ > 0)
    {
        if (!resize_swap_chain(width_, height_))
        {
            window_closed_ = true;
            return;
        }
        resize_pending_ = false;
    }

    if (rml_console_controller_ != nullptr)
    {
        rml_console_controller_->update();
    }

    sync_main_menu_visibility();
    if (rml_hud_controller_ != nullptr)
    {
        const bool main_menu_visible = main_menu_controller_ != nullptr && main_menu_controller_->visible();
        rml_hud_controller_->update(main_menu_visible);
    }

    if (rml_context_ != nullptr)
    {
        rml_context_->Update();
    }

    const std::uint32_t frame = frame_index_;
    ID3D12CommandAllocator* allocator = command_allocators_[frame].Get();
    allocator->Reset();
    command_list_->Reset(allocator, nullptr);

    D3D12_RESOURCE_BARRIER begin_barrier{};
    begin_barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    begin_barrier.Transition.pResource = render_targets_[frame].Get();
    begin_barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
    begin_barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
    begin_barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    command_list_->ResourceBarrier(1, &begin_barrier);

    const float pulse = static_cast<float>((context.frame_index % 120) / 120.0);
    const std::array<float, 4> clear_color{0.03F, 0.05F + (0.05F * pulse), 0.09F + (0.15F * pulse), 1.0F};

    D3D12_CPU_DESCRIPTOR_HANDLE rtv_handle = rtv_heap_->GetCPUDescriptorHandleForHeapStart();
    rtv_handle.ptr += static_cast<SIZE_T>(frame) * rtv_descriptor_size_;
    D3D12_CPU_DESCRIPTOR_HANDLE dsv_handle = dsv_heap_->GetCPUDescriptorHandleForHeapStart();
    command_list_->OMSetRenderTargets(1, &rtv_handle, FALSE, &dsv_handle);
    command_list_->ClearRenderTargetView(rtv_handle, clear_color.data(), 0, nullptr);
    command_list_->ClearDepthStencilView(dsv_handle, D3D12_CLEAR_FLAG_DEPTH, 1.0F, 0, 0, nullptr);

    const LoadedWorld* active_world = engine_context_ != nullptr ? engine_context_->world.current_world() : nullptr;
    if (active_world != nullptr)
    {
        render_skybox(*active_world);
    }
    else
    {
        skybox_gpu_.reset();
        skybox_gpu_generation_ = engine_context_ != nullptr ? engine_context_->world.generation() : 0;
    }

    render_world();

    if (rml_context_ != nullptr && rml_render_interface_)
    {
        rml_render_interface_->begin_frame(command_list_.Get(), frame, width_, height_);
        rml_context_->Render();
        rml_render_interface_->end_frame();
    }

    D3D12_RESOURCE_BARRIER end_barrier = begin_barrier;
    end_barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
    end_barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
    command_list_->ResourceBarrier(1, &end_barrier);

    command_list_->Close();
    ID3D12CommandList* lists[] = {command_list_.Get()};
    command_queue_->ExecuteCommandLists(1, lists);

    const UINT present_sync = vsync_ ? 1U : 0U;
    const UINT present_flags = (!vsync_ && allow_tearing_) ? DXGI_PRESENT_ALLOW_TEARING : 0U;
    const HRESULT present_result = swap_chain_->Present(present_sync, present_flags);
    if (FAILED(present_result))
    {
        log_error("{}", hresult_message("IDXGISwapChain::Present", present_result));
        window_closed_ = true;
        return;
    }

    move_to_next_frame();
}

bool Dx12Renderer::should_close() const
{
    return window_closed_ || (main_menu_controller_ != nullptr && main_menu_controller_->should_quit());
}

void Dx12Renderer::shutdown()
{
    if (!initialized_ && window_ == nullptr && fence_event_ == nullptr && !rml_initialized_)
    {
        return;
    }

    if (initialized_)
    {
        wait_for_gpu();
    }

    shutdown_rml();

    render_targets_ = {};
    depth_buffer_.Reset();
    command_allocators_ = {};
    world_gpu_.reset();
    skybox_gpu_.reset();
    skybox_pipeline_state_.Reset();
    skybox_root_signature_.Reset();
    world_pipeline_state_.Reset();
    world_root_signature_.Reset();
    command_list_.Reset();
    rtv_heap_.Reset();
    dsv_heap_.Reset();
    swap_chain_.Reset();
    command_queue_.Reset();
    fence_.Reset();
    device_.Reset();

    if (fence_event_ != nullptr)
    {
        CloseHandle(fence_event_);
        fence_event_ = nullptr;
    }

    if (window_ != nullptr)
    {
        SDL_DestroyWindow(window_);
        window_ = nullptr;
        hwnd_ = nullptr;
    }

    if (sdl_initialized_)
    {
        SDL_QuitSubSystem(SDL_INIT_VIDEO);
        SDL_Quit();
        sdl_initialized_ = false;
    }

    initialized_ = false;
}

bool Dx12Renderer::create_window(const RuntimeConfig&)
{
    SDL_SetMainReady();
    if (!SDL_Init(SDL_INIT_VIDEO))
    {
        log_error("SDL_Init failed: {}", SDL_GetError());
        return false;
    }
    sdl_initialized_ = true;

    const SDL_WindowFlags flags = SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY;
    window_ = SDL_CreateWindow(kWindowTitle, static_cast<int>(width_), static_cast<int>(height_), flags);
    if (window_ == nullptr)
    {
        log_error("SDL_CreateWindow failed: {}", SDL_GetError());
        return false;
    }

    int pixel_width = 0;
    int pixel_height = 0;
    if (SDL_GetWindowSizeInPixels(window_, &pixel_width, &pixel_height) && pixel_width > 0 && pixel_height > 0)
    {
        width_ = static_cast<std::uint32_t>(pixel_width);
        height_ = static_cast<std::uint32_t>(pixel_height);
    }

    SDL_PropertiesID properties = SDL_GetWindowProperties(window_);
    hwnd_ = static_cast<HWND>(SDL_GetPointerProperty(properties, SDL_PROP_WINDOW_WIN32_HWND_POINTER, nullptr));
    if (hwnd_ == nullptr)
    {
        log_error("SDL window did not expose a Win32 HWND");
        return false;
    }

    SDL_ShowWindow(window_);

    return true;
}

bool Dx12Renderer::create_device_objects(const RuntimeConfig&)
{
#if defined(_DEBUG)
    ComPtr<ID3D12Debug> debug_controller;
    if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debug_controller))))
    {
        debug_controller->EnableDebugLayer();
    }
#endif

    UINT factory_flags = 0;
#if defined(_DEBUG)
    factory_flags = DXGI_CREATE_FACTORY_DEBUG;
#endif

    ComPtr<IDXGIFactory6> factory;
    if (!succeeded(CreateDXGIFactory2(factory_flags, IID_PPV_ARGS(&factory)), "CreateDXGIFactory2"))
    {
        return false;
    }

    ComPtr<IDXGIFactory5> factory5;
    if (SUCCEEDED(factory.As(&factory5)))
    {
        allow_tearing_ = query_tearing_support(factory5.Get());
    }

    ComPtr<IDXGIAdapter1> adapter = choose_hardware_adapter(factory.Get());
    if (!adapter)
    {
        log_error("no suitable DX12 hardware adapter found");
        return false;
    }

    if (!succeeded(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device_)), "D3D12CreateDevice"))
    {
        return false;
    }

    D3D12_COMMAND_QUEUE_DESC queue_desc{};
    queue_desc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    queue_desc.Flags = D3D12_COMMAND_QUEUE_FLAG_NONE;
    if (!succeeded(device_->CreateCommandQueue(&queue_desc, IID_PPV_ARGS(&command_queue_)), "ID3D12Device::CreateCommandQueue"))
    {
        return false;
    }

    DXGI_SWAP_CHAIN_DESC1 swap_chain_desc{};
    swap_chain_desc.BufferCount = kFrameCount;
    swap_chain_desc.Width = width_;
    swap_chain_desc.Height = height_;
    swap_chain_desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    swap_chain_desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    swap_chain_desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    swap_chain_desc.SampleDesc.Count = 1;
    swap_chain_desc.Flags = allow_tearing_ ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING : 0U;

    ComPtr<IDXGISwapChain1> swap_chain1;
    if (!succeeded(factory->CreateSwapChainForHwnd(command_queue_.Get(), hwnd_, &swap_chain_desc, nullptr, nullptr, &swap_chain1),
            "IDXGIFactory::CreateSwapChainForHwnd"))
    {
        return false;
    }

    factory->MakeWindowAssociation(hwnd_, DXGI_MWA_NO_ALT_ENTER);
    if (!succeeded(swap_chain1.As(&swap_chain_), "IDXGISwapChain1::QueryInterface IDXGISwapChain3"))
    {
        return false;
    }

    frame_index_ = swap_chain_->GetCurrentBackBufferIndex();

    D3D12_DESCRIPTOR_HEAP_DESC heap_desc{};
    heap_desc.NumDescriptors = kFrameCount;
    heap_desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    if (!succeeded(device_->CreateDescriptorHeap(&heap_desc, IID_PPV_ARGS(&rtv_heap_)), "ID3D12Device::CreateDescriptorHeap RTV"))
    {
        return false;
    }

    rtv_descriptor_size_ = device_->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    dsv_descriptor_size_ = device_->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_DSV);

    for (std::uint32_t frame = 0; frame < kFrameCount; ++frame)
    {
        if (!succeeded(device_->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&command_allocators_[frame])),
                "ID3D12Device::CreateCommandAllocator"))
        {
            return false;
        }
    }

    if (!succeeded(device_->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, command_allocators_[frame_index_].Get(), nullptr,
            IID_PPV_ARGS(&command_list_)), "ID3D12Device::CreateCommandList"))
    {
        return false;
    }
    command_list_->Close();

    if (!succeeded(device_->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence_)), "ID3D12Device::CreateFence"))
    {
        return false;
    }

    fence_value_ = 1;
    fence_event_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (fence_event_ == nullptr)
    {
        log_error("CreateEventW failed for DX12 fence");
        return false;
    }

    return true;
}

bool Dx12Renderer::create_render_targets()
{
    D3D12_CPU_DESCRIPTOR_HANDLE rtv_handle = rtv_heap_->GetCPUDescriptorHandleForHeapStart();

    for (std::uint32_t frame = 0; frame < kFrameCount; ++frame)
    {
        if (!succeeded(swap_chain_->GetBuffer(frame, IID_PPV_ARGS(&render_targets_[frame])), "IDXGISwapChain::GetBuffer"))
        {
            return false;
        }

        device_->CreateRenderTargetView(render_targets_[frame].Get(), nullptr, rtv_handle);
        rtv_handle.ptr += rtv_descriptor_size_;
    }

    return true;
}

bool Dx12Renderer::create_depth_buffer()
{
    if (!device_ || width_ == 0 || height_ == 0)
    {
        return true;
    }

    if (!dsv_heap_)
    {
        D3D12_DESCRIPTOR_HEAP_DESC heap_desc{};
        heap_desc.NumDescriptors = 1;
        heap_desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
        if (!succeeded(device_->CreateDescriptorHeap(&heap_desc, IID_PPV_ARGS(&dsv_heap_)), "ID3D12Device::CreateDescriptorHeap DSV"))
        {
            return false;
        }
    }

    D3D12_CLEAR_VALUE clear_value{};
    clear_value.Format = kDepthFormat;
    clear_value.DepthStencil.Depth = 1.0F;
    clear_value.DepthStencil.Stencil = 0;

    D3D12_RESOURCE_DESC desc = texture2d_desc(width_, height_, 1, kDepthFormat);
    desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;
    const auto default_heap = heap_properties(D3D12_HEAP_TYPE_DEFAULT);
    if (!succeeded(device_->CreateCommittedResource(&default_heap,
            D3D12_HEAP_FLAG_NONE,
            &desc,
            D3D12_RESOURCE_STATE_DEPTH_WRITE,
            &clear_value,
            IID_PPV_ARGS(&depth_buffer_)),
            "ID3D12Device::CreateCommittedResource depth buffer"))
    {
        return false;
    }

    device_->CreateDepthStencilView(depth_buffer_.Get(), nullptr, dsv_heap_->GetCPUDescriptorHandleForHeapStart());
    return true;
}

bool Dx12Renderer::create_world_pipeline()
{
    D3D12_DESCRIPTOR_RANGE texture_range{};
    texture_range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    texture_range.NumDescriptors = 1;
    texture_range.BaseShaderRegister = 0;
    texture_range.RegisterSpace = 0;
    texture_range.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

    std::array<D3D12_ROOT_PARAMETER, 3> root_parameters{};
    root_parameters[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    root_parameters[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_VERTEX;
    root_parameters[0].Constants.ShaderRegister = 0;
    root_parameters[0].Constants.RegisterSpace = 0;
    root_parameters[0].Constants.Num32BitValues = 16;

    root_parameters[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    root_parameters[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    root_parameters[1].DescriptorTable.NumDescriptorRanges = 1;
    root_parameters[1].DescriptorTable.pDescriptorRanges = &texture_range;

    root_parameters[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    root_parameters[2].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    root_parameters[2].Constants.ShaderRegister = 1;
    root_parameters[2].Constants.RegisterSpace = 0;
    root_parameters[2].Constants.Num32BitValues = static_cast<UINT>(sizeof(MaterialGpuConstants) / sizeof(std::uint32_t));

    D3D12_STATIC_SAMPLER_DESC sampler{};
    sampler.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    sampler.AddressU = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    sampler.AddressV = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    sampler.MipLODBias = 0.0F;
    sampler.MaxAnisotropy = 1;
    sampler.ComparisonFunc = D3D12_COMPARISON_FUNC_ALWAYS;
    sampler.BorderColor = D3D12_STATIC_BORDER_COLOR_OPAQUE_WHITE;
    sampler.MinLOD = 0.0F;
    sampler.MaxLOD = D3D12_FLOAT32_MAX;
    sampler.ShaderRegister = 0;
    sampler.RegisterSpace = 0;
    sampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    D3D12_ROOT_SIGNATURE_DESC root_desc{};
    root_desc.NumParameters = static_cast<UINT>(root_parameters.size());
    root_desc.pParameters = root_parameters.data();
    root_desc.NumStaticSamplers = 1;
    root_desc.pStaticSamplers = &sampler;
    root_desc.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;

    ComPtr<ID3DBlob> signature_blob;
    ComPtr<ID3DBlob> signature_errors;
    const HRESULT serialize_result = D3D12SerializeRootSignature(&root_desc, D3D_ROOT_SIGNATURE_VERSION_1, &signature_blob, &signature_errors);
    if (FAILED(serialize_result))
    {
        if (signature_errors)
        {
            log_error("world root signature error: {}", static_cast<const char*>(signature_errors->GetBufferPointer()));
        }
        else
        {
            log_error("{}", hresult_message("D3D12SerializeRootSignature", serialize_result));
        }
        return false;
    }

    if (!succeeded(device_->CreateRootSignature(0, signature_blob->GetBufferPointer(), signature_blob->GetBufferSize(),
            IID_PPV_ARGS(&world_root_signature_)), "ID3D12Device::CreateRootSignature world"))
    {
        return false;
    }

    ComPtr<ID3DBlob> vertex_shader = compile_shader(world_material_vertex_shader_source(), "main", "vs_5_0");
    ComPtr<ID3DBlob> pixel_shader = compile_shader(world_material_pixel_shader_source(), "main", "ps_5_0");
    if (!vertex_shader || !pixel_shader)
    {
        return false;
    }

    std::array<D3D12_INPUT_ELEMENT_DESC, 3> input_layout{{
        {"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, offsetof(WorldDrawVertex, position), D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
        {"NORMAL", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, offsetof(WorldDrawVertex, normal), D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
        {"TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, offsetof(WorldDrawVertex, texcoord), D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
    }};

    D3D12_GRAPHICS_PIPELINE_STATE_DESC pso_desc{};
    pso_desc.InputLayout = {input_layout.data(), static_cast<UINT>(input_layout.size())};
    pso_desc.pRootSignature = world_root_signature_.Get();
    pso_desc.VS = {vertex_shader->GetBufferPointer(), vertex_shader->GetBufferSize()};
    pso_desc.PS = {pixel_shader->GetBufferPointer(), pixel_shader->GetBufferSize()};
    pso_desc.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    pso_desc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    pso_desc.RasterizerState.FrontCounterClockwise = FALSE;
    pso_desc.RasterizerState.DepthBias = D3D12_DEFAULT_DEPTH_BIAS;
    pso_desc.RasterizerState.DepthBiasClamp = D3D12_DEFAULT_DEPTH_BIAS_CLAMP;
    pso_desc.RasterizerState.SlopeScaledDepthBias = D3D12_DEFAULT_SLOPE_SCALED_DEPTH_BIAS;
    pso_desc.RasterizerState.DepthClipEnable = TRUE;
    pso_desc.BlendState.RenderTarget[0].BlendEnable = FALSE;
    pso_desc.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    pso_desc.DepthStencilState.DepthEnable = TRUE;
    pso_desc.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
    pso_desc.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_LESS_EQUAL;
    pso_desc.DepthStencilState.StencilEnable = FALSE;
    pso_desc.SampleMask = std::numeric_limits<UINT>::max();
    pso_desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    pso_desc.NumRenderTargets = 1;
    pso_desc.RTVFormats[0] = kRenderTargetFormat;
    pso_desc.DSVFormat = kDepthFormat;
    pso_desc.SampleDesc.Count = 1;

    return succeeded(device_->CreateGraphicsPipelineState(&pso_desc, IID_PPV_ARGS(&world_pipeline_state_)),
        "ID3D12Device::CreateGraphicsPipelineState world");
}

bool Dx12Renderer::create_skybox_pipeline()
{
    D3D12_DESCRIPTOR_RANGE texture_range{};
    texture_range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    texture_range.NumDescriptors = 1;
    texture_range.BaseShaderRegister = 0;
    texture_range.RegisterSpace = 0;
    texture_range.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

    std::array<D3D12_ROOT_PARAMETER, 2> root_parameters{};
    root_parameters[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    root_parameters[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_VERTEX;
    root_parameters[0].Constants.ShaderRegister = 0;
    root_parameters[0].Constants.RegisterSpace = 0;
    root_parameters[0].Constants.Num32BitValues = 16;

    root_parameters[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    root_parameters[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    root_parameters[1].DescriptorTable.NumDescriptorRanges = 1;
    root_parameters[1].DescriptorTable.pDescriptorRanges = &texture_range;

    D3D12_STATIC_SAMPLER_DESC sampler{};
    sampler.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    sampler.AddressU = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampler.AddressV = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampler.MipLODBias = 0.0F;
    sampler.MaxAnisotropy = 1;
    sampler.ComparisonFunc = D3D12_COMPARISON_FUNC_ALWAYS;
    sampler.BorderColor = D3D12_STATIC_BORDER_COLOR_OPAQUE_WHITE;
    sampler.MinLOD = 0.0F;
    sampler.MaxLOD = D3D12_FLOAT32_MAX;
    sampler.ShaderRegister = 0;
    sampler.RegisterSpace = 0;
    sampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    D3D12_ROOT_SIGNATURE_DESC root_desc{};
    root_desc.NumParameters = static_cast<UINT>(root_parameters.size());
    root_desc.pParameters = root_parameters.data();
    root_desc.NumStaticSamplers = 1;
    root_desc.pStaticSamplers = &sampler;
    root_desc.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;

    ComPtr<ID3DBlob> signature_blob;
    ComPtr<ID3DBlob> signature_errors;
    const HRESULT serialize_result = D3D12SerializeRootSignature(&root_desc, D3D_ROOT_SIGNATURE_VERSION_1, &signature_blob, &signature_errors);
    if (FAILED(serialize_result))
    {
        if (signature_errors)
        {
            log_error("skybox root signature error: {}", static_cast<const char*>(signature_errors->GetBufferPointer()));
        }
        else
        {
            log_error("{}", hresult_message("D3D12SerializeRootSignature skybox", serialize_result));
        }
        return false;
    }

    if (!succeeded(device_->CreateRootSignature(0, signature_blob->GetBufferPointer(), signature_blob->GetBufferSize(),
            IID_PPV_ARGS(&skybox_root_signature_)), "ID3D12Device::CreateRootSignature skybox"))
    {
        return false;
    }

    ComPtr<ID3DBlob> vertex_shader = compile_shader(skybox_vertex_shader_source(), "main", "vs_5_0");
    ComPtr<ID3DBlob> pixel_shader = compile_shader(skybox_pixel_shader_source(), "main", "ps_5_0");
    if (!vertex_shader || !pixel_shader)
    {
        return false;
    }

    std::array<D3D12_INPUT_ELEMENT_DESC, 2> input_layout{{
        {"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, offsetof(SkyboxDrawVertex, position), D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
        {"TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, offsetof(SkyboxDrawVertex, texcoord), D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
    }};

    D3D12_GRAPHICS_PIPELINE_STATE_DESC pso_desc{};
    pso_desc.InputLayout = {input_layout.data(), static_cast<UINT>(input_layout.size())};
    pso_desc.pRootSignature = skybox_root_signature_.Get();
    pso_desc.VS = {vertex_shader->GetBufferPointer(), vertex_shader->GetBufferSize()};
    pso_desc.PS = {pixel_shader->GetBufferPointer(), pixel_shader->GetBufferSize()};
    pso_desc.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    pso_desc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    pso_desc.RasterizerState.FrontCounterClockwise = FALSE;
    pso_desc.RasterizerState.DepthBias = D3D12_DEFAULT_DEPTH_BIAS;
    pso_desc.RasterizerState.DepthBiasClamp = D3D12_DEFAULT_DEPTH_BIAS_CLAMP;
    pso_desc.RasterizerState.SlopeScaledDepthBias = D3D12_DEFAULT_SLOPE_SCALED_DEPTH_BIAS;
    pso_desc.RasterizerState.DepthClipEnable = TRUE;
    pso_desc.BlendState.RenderTarget[0].BlendEnable = FALSE;
    pso_desc.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    pso_desc.DepthStencilState.DepthEnable = FALSE;
    pso_desc.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
    pso_desc.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_ALWAYS;
    pso_desc.DepthStencilState.StencilEnable = FALSE;
    pso_desc.SampleMask = std::numeric_limits<UINT>::max();
    pso_desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    pso_desc.NumRenderTargets = 1;
    pso_desc.RTVFormats[0] = kRenderTargetFormat;
    pso_desc.DSVFormat = kDepthFormat;
    pso_desc.SampleDesc.Count = 1;

    return succeeded(device_->CreateGraphicsPipelineState(&pso_desc, IID_PPV_ARGS(&skybox_pipeline_state_)),
        "ID3D12Device::CreateGraphicsPipelineState skybox");
}

bool Dx12Renderer::resize_swap_chain(std::uint32_t width, std::uint32_t height)
{
    if (!swap_chain_ || width == 0 || height == 0)
    {
        return true;
    }

    wait_for_gpu();
    render_targets_ = {};
    depth_buffer_.Reset();

    const UINT flags = allow_tearing_ ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING : 0U;
    if (!succeeded(swap_chain_->ResizeBuffers(kFrameCount, width, height, DXGI_FORMAT_R8G8B8A8_UNORM, flags), "IDXGISwapChain::ResizeBuffers"))
    {
        return false;
    }

    frame_index_ = swap_chain_->GetCurrentBackBufferIndex();
    return create_render_targets() && create_depth_buffer();
}

bool Dx12Renderer::initialize_rml(const RuntimeConfig& config)
{
    rml_system_interface_ = std::make_unique<SystemInterface_SDL>();
    rml_system_interface_->SetWindow(window_);

    rml_render_interface_ = std::make_unique<RmlDx12RenderInterface>();
    if (!rml_render_interface_->initialize(device_.Get(), command_queue_.Get()))
    {
        return false;
    }

    Rml::SetSystemInterface(rml_system_interface_.get());
    Rml::SetRenderInterface(rml_render_interface_.get());

    if (!Rml::Initialise())
    {
        log_error("Rml::Initialise failed");
        return false;
    }
    rml_initialized_ = true;

    rml_context_ = Rml::CreateContext("main", Rml::Vector2i(static_cast<int>(width_), static_cast<int>(height_)));
    if (rml_context_ == nullptr)
    {
        log_error("Rml::CreateContext failed");
        return false;
    }

    Rml::Debugger::Initialise(rml_context_);
    Rml::Debugger::SetVisible(false);
    main_menu_controller_ = std::make_unique<MainMenuController>();
    main_menu_controller_->set_open_console_callback([this] {
        if (rml_console_controller_ != nullptr)
        {
            rml_console_controller_->show();
        }
    });
    main_menu_controller_->set_launch_map_callback([this](std::string map_name) {
        if (engine_context_ != nullptr)
        {
            engine_context_->command_buffer.add_text("map " + map_name);
        }
    });

    const std::vector<std::filesystem::path> font_roots{
        config.content_root / "csgo/resource/ui/fonts",
        config.content_root / "assets/ui/fonts",
        config.content_root / "resource/ui/fonts",
    };

    int loaded_fonts = 0;
    for (const std::filesystem::path& font_root : font_roots)
    {
        std::error_code error;
        if (!std::filesystem::is_directory(font_root, error))
        {
            continue;
        }

        for (const std::filesystem::directory_entry& entry : std::filesystem::directory_iterator(font_root, error))
        {
            if (error)
            {
                break;
            }

            if (!entry.is_regular_file(error))
            {
                continue;
            }

            std::string extension = entry.path().extension().string();
            std::transform(extension.begin(), extension.end(), extension.begin(), [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
            if (extension != ".ttf" && extension != ".otf")
            {
                continue;
            }

            if (Rml::LoadFontFace(rml_path_string(entry.path()), loaded_fonts == 0))
            {
                ++loaded_fonts;
            }
        }
    }

    log_info("loaded {} RmlUi font face(s) from '{}'", loaded_fonts, rml_path_string(config.content_root));
    if (!main_menu_controller_->initialize(*rml_context_, config))
    {
        return false;
    }

    if (engine_context_ != nullptr)
    {
        rml_hud_controller_ = std::make_unique<RmlHudController>();
        if (!rml_hud_controller_->initialize(*rml_context_, *engine_context_, config))
        {
            return false;
        }

        rml_console_controller_ = std::make_unique<RmlConsoleController>();
        if (!rml_console_controller_->initialize(*rml_context_, *engine_context_))
        {
            return false;
        }
    }

    return true;
}

void Dx12Renderer::sync_main_menu_visibility()
{
    if (engine_context_ == nullptr || main_menu_controller_ == nullptr)
    {
        return;
    }

    const std::uint64_t world_generation = engine_context_->world.generation();
    if (world_generation == main_menu_world_generation_)
    {
        return;
    }

    main_menu_world_generation_ = world_generation;
    if (engine_context_->world.current_world() != nullptr)
    {
        if (main_menu_controller_->visible())
        {
            main_menu_controller_->set_visible(false);
            log_info("hid RmlUi main menu for active world");
        }
    }
    else if (!main_menu_controller_->visible())
    {
        main_menu_controller_->set_visible(true);
        log_info("showed RmlUi main menu because no world is active");
    }
}

void Dx12Renderer::render_skybox(const LoadedWorld& world)
{
    if (engine_context_ == nullptr || skybox_pipeline_state_ == nullptr || skybox_root_signature_ == nullptr || command_list_ == nullptr)
    {
        return;
    }

    const std::uint64_t generation = engine_context_->world.generation();
    if (world.kind != WorldAssetKind::SourceBsp || !world.mesh.has_sky_surfaces)
    {
        if (skybox_gpu_generation_ != generation)
        {
            skybox_gpu_.reset();
            skybox_gpu_generation_ = generation;
        }
        return;
    }

    const std::optional<std::string> requested_sky_name = skybox_name_from_world(world);
    if (!requested_sky_name || requested_sky_name->empty())
    {
        if (skybox_gpu_generation_ != generation)
        {
            skybox_gpu_.reset();
            skybox_gpu_generation_ = generation;
        }
        return;
    }

    if (skybox_gpu_generation_ != generation || (skybox_gpu_ != nullptr && skybox_gpu_->sky_name != *requested_sky_name))
    {
        skybox_gpu_.reset();
        skybox_gpu_generation_ = generation;

        static constexpr std::array<std::string_view, kSkyboxFaceCount> kSkyboxSuffixByDrawAxis{"rt", "lf", "bk", "ft", "up", "dn"};
        SourceAssetStore source_assets(engine_context_->filesystem, &world.embedded_assets);
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
            log_warning("skybox '{}' is incomplete; trying default sky_urb01", resolved_sky_name);
            resolved_sky_name = "sky_urb01";
            sky_textures = load_skybox_textures(resolved_sky_name);
        }

        if (!sky_textures)
        {
            log_warning("skybox '{}' is missing one or more Source VTF faces; sky draw disabled", resolved_sky_name);
            return;
        }

        auto gpu = std::make_unique<SkyboxGpuResources>();
        gpu->sky_name = *requested_sky_name;

        const std::array<SkyboxDrawVertex, kSkyboxFaceCount * 4> vertices = build_skybox_vertices();
        const std::array<std::uint16_t, kSkyboxFaceCount * 6> indices = build_skybox_indices();
        const auto vertex_bytes = static_cast<std::uint64_t>(vertices.size() * sizeof(SkyboxDrawVertex));
        const auto index_bytes = static_cast<std::uint64_t>(indices.size() * sizeof(std::uint16_t));
        const auto upload_heap = heap_properties(D3D12_HEAP_TYPE_UPLOAD);

        const auto vertex_desc = buffer_desc(vertex_bytes);
        if (!succeeded(device_->CreateCommittedResource(&upload_heap, D3D12_HEAP_FLAG_NONE, &vertex_desc, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                IID_PPV_ARGS(&gpu->vertex_buffer)), "ID3D12Device::CreateCommittedResource skybox vertex buffer"))
        {
            return;
        }

        const auto index_desc = buffer_desc(index_bytes);
        if (!succeeded(device_->CreateCommittedResource(&upload_heap, D3D12_HEAP_FLAG_NONE, &index_desc, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                IID_PPV_ARGS(&gpu->index_buffer)), "ID3D12Device::CreateCommittedResource skybox index buffer"))
        {
            return;
        }

        void* mapped = nullptr;
        if (!succeeded(gpu->vertex_buffer->Map(0, nullptr, &mapped), "ID3D12Resource::Map skybox vertex buffer"))
        {
            return;
        }
        std::memcpy(mapped, vertices.data(), static_cast<std::size_t>(vertex_bytes));
        gpu->vertex_buffer->Unmap(0, nullptr);

        if (!succeeded(gpu->index_buffer->Map(0, nullptr, &mapped), "ID3D12Resource::Map skybox index buffer"))
        {
            return;
        }
        std::memcpy(mapped, indices.data(), static_cast<std::size_t>(index_bytes));
        gpu->index_buffer->Unmap(0, nullptr);

        gpu->vertex_view.BufferLocation = gpu->vertex_buffer->GetGPUVirtualAddress();
        gpu->vertex_view.SizeInBytes = static_cast<UINT>(vertex_bytes);
        gpu->vertex_view.StrideInBytes = sizeof(SkyboxDrawVertex);
        gpu->index_view.BufferLocation = gpu->index_buffer->GetGPUVirtualAddress();
        gpu->index_view.SizeInBytes = static_cast<UINT>(index_bytes);
        gpu->index_view.Format = DXGI_FORMAT_R16_UINT;

        D3D12_DESCRIPTOR_HEAP_DESC srv_heap_desc{};
        srv_heap_desc.NumDescriptors = kSkyboxFaceCount;
        srv_heap_desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
        srv_heap_desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        if (!succeeded(device_->CreateDescriptorHeap(&srv_heap_desc, IID_PPV_ARGS(&gpu->srv_heap)), "ID3D12Device::CreateDescriptorHeap skybox SRV"))
        {
            return;
        }
        gpu->srv_descriptor_size = device_->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

        D3D12_CPU_DESCRIPTOR_HANDLE cpu_handle = gpu->srv_heap->GetCPUDescriptorHandleForHeapStart();
        for (std::size_t face = 0; face < kSkyboxFaceCount; ++face)
        {
            if (!upload_source_texture_to_gpu(device_.Get(), command_list_.Get(), (*sky_textures)[face], cpu_handle, gpu->textures[face]))
            {
                log_warning("failed to upload skybox '{}' face '{}'", resolved_sky_name, kSkyboxSuffixByDrawAxis[face]);
                return;
            }
            cpu_handle.ptr += gpu->srv_descriptor_size;
        }

        log_info("uploaded skybox '{}' from Source faces", resolved_sky_name);
        skybox_gpu_ = std::move(gpu);
    }

    if (skybox_gpu_ == nullptr)
    {
        return;
    }

    D3D12_VIEWPORT viewport{};
    viewport.TopLeftX = 0.0F;
    viewport.TopLeftY = 0.0F;
    viewport.Width = static_cast<float>(width_);
    viewport.Height = static_cast<float>(height_);
    viewport.MinDepth = 0.0F;
    viewport.MaxDepth = 1.0F;

    D3D12_RECT scissor{};
    scissor.left = 0;
    scissor.top = 0;
    scissor.right = static_cast<LONG>(width_);
    scissor.bottom = static_cast<LONG>(height_);

    const std::array<float, 16> constants = skybox_to_clip_matrix(&engine_context_->camera, width_, height_);
    ID3D12DescriptorHeap* descriptor_heaps[] = {skybox_gpu_->srv_heap.Get()};
    command_list_->SetDescriptorHeaps(1, descriptor_heaps);
    command_list_->SetPipelineState(skybox_pipeline_state_.Get());
    command_list_->SetGraphicsRootSignature(skybox_root_signature_.Get());
    command_list_->SetGraphicsRoot32BitConstants(0, static_cast<UINT>(constants.size()), constants.data(), 0);
    command_list_->RSSetViewports(1, &viewport);
    command_list_->RSSetScissorRects(1, &scissor);
    command_list_->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    command_list_->IASetVertexBuffers(0, 1, &skybox_gpu_->vertex_view);
    command_list_->IASetIndexBuffer(&skybox_gpu_->index_view);

    const D3D12_GPU_DESCRIPTOR_HANDLE gpu_start = skybox_gpu_->srv_heap->GetGPUDescriptorHandleForHeapStart();
    for (std::uint32_t face = 0; face < kSkyboxFaceCount; ++face)
    {
        D3D12_GPU_DESCRIPTOR_HANDLE texture_handle = gpu_start;
        texture_handle.ptr += static_cast<UINT64>(face) * skybox_gpu_->srv_descriptor_size;
        command_list_->SetGraphicsRootDescriptorTable(1, texture_handle);
        command_list_->DrawIndexedInstanced(6, 1, face * 6, 0, 0);
    }
}

void Dx12Renderer::render_world()
{
    if (engine_context_ == nullptr || world_pipeline_state_ == nullptr || world_root_signature_ == nullptr || command_list_ == nullptr)
    {
        return;
    }

    const LoadedWorld* world = engine_context_->world.current_world();
    const std::uint64_t generation = engine_context_->world.generation();
    if (world == nullptr || world->mesh.vertices.empty() || world->mesh.indices.empty())
    {
        world_gpu_.reset();
        world_gpu_generation_ = generation;
        return;
    }

    if (world_gpu_ == nullptr || world_gpu_generation_ != generation)
    {
        std::vector<WorldDrawVertex> vertices;
        vertices.reserve(world->mesh.vertices.size());
        for (const WorldMeshVertex& vertex : world->mesh.vertices)
        {
            vertices.push_back(WorldDrawVertex{
                {vertex.position.x, vertex.position.y, vertex.position.z},
                {vertex.normal.x, vertex.normal.y, vertex.normal.z},
                {vertex.texcoord.x, vertex.texcoord.y},
            });
        }

        const auto vertex_bytes = static_cast<std::uint64_t>(vertices.size() * sizeof(WorldDrawVertex));
        const auto index_bytes = static_cast<std::uint64_t>(world->mesh.indices.size() * sizeof(std::uint32_t));
        const auto upload_heap = heap_properties(D3D12_HEAP_TYPE_UPLOAD);
        auto gpu = std::make_unique<WorldGpuResources>();

        const auto vertex_desc = buffer_desc(vertex_bytes);
        if (!succeeded(device_->CreateCommittedResource(&upload_heap, D3D12_HEAP_FLAG_NONE, &vertex_desc, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                IID_PPV_ARGS(&gpu->vertex_buffer)), "ID3D12Device::CreateCommittedResource world vertex buffer"))
        {
            return;
        }

        const auto index_desc = buffer_desc(index_bytes);
        if (!succeeded(device_->CreateCommittedResource(&upload_heap, D3D12_HEAP_FLAG_NONE, &index_desc, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                IID_PPV_ARGS(&gpu->index_buffer)), "ID3D12Device::CreateCommittedResource world index buffer"))
        {
            return;
        }

        void* mapped = nullptr;
        if (!succeeded(gpu->vertex_buffer->Map(0, nullptr, &mapped), "ID3D12Resource::Map world vertex buffer"))
        {
            return;
        }
        std::memcpy(mapped, vertices.data(), static_cast<std::size_t>(vertex_bytes));
        gpu->vertex_buffer->Unmap(0, nullptr);

        if (!succeeded(gpu->index_buffer->Map(0, nullptr, &mapped), "ID3D12Resource::Map world index buffer"))
        {
            return;
        }
        std::memcpy(mapped, world->mesh.indices.data(), static_cast<std::size_t>(index_bytes));
        gpu->index_buffer->Unmap(0, nullptr);

        gpu->vertex_view.BufferLocation = gpu->vertex_buffer->GetGPUVirtualAddress();
        gpu->vertex_view.SizeInBytes = static_cast<UINT>(vertex_bytes);
        gpu->vertex_view.StrideInBytes = sizeof(WorldDrawVertex);
        gpu->index_view.BufferLocation = gpu->index_buffer->GetGPUVirtualAddress();
        gpu->index_view.SizeInBytes = static_cast<UINT>(index_bytes);
        gpu->index_view.Format = DXGI_FORMAT_R32_UINT;
        gpu->index_count = static_cast<std::uint32_t>(world->mesh.indices.size());

        const std::uint32_t material_count = std::max<std::uint32_t>(static_cast<std::uint32_t>(world->mesh.materials.size()), 1U);
        D3D12_DESCRIPTOR_HEAP_DESC srv_heap_desc{};
        srv_heap_desc.NumDescriptors = material_count;
        srv_heap_desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
        srv_heap_desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        if (!succeeded(device_->CreateDescriptorHeap(&srv_heap_desc, IID_PPV_ARGS(&gpu->srv_heap)), "ID3D12Device::CreateDescriptorHeap world SRV"))
        {
            return;
        }
        gpu->srv_descriptor_size = device_->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        gpu->textures.reserve(material_count);
        gpu->material_constants.reserve(material_count);

        SourceAssetStore source_assets(engine_context_->filesystem, &world->embedded_assets);
        MaterialSystem material_system(source_assets);
        D3D12_CPU_DESCRIPTOR_HANDLE cpu_handle = gpu->srv_heap->GetCPUDescriptorHandleForHeapStart();
        for (std::uint32_t material_index = 0; material_index < material_count; ++material_index)
        {
            const WorldMaterial fallback_material{.name = "__missing", .width = 1, .height = 1};
            const WorldMaterial& material = material_index < world->mesh.materials.size() ? world->mesh.materials[material_index] : fallback_material;
            LoadedMaterial loaded_material = material_system.load_world_material(material.name);

            TextureGpuResources gpu_texture;
            if (!upload_source_texture_to_gpu(device_.Get(), command_list_.Get(), loaded_material.base_texture, cpu_handle, gpu_texture))
            {
                SourceTexture fallback = material_system.fallback_texture(material.name);
                if (!upload_source_texture_to_gpu(device_.Get(), command_list_.Get(), fallback, cpu_handle, gpu_texture))
                {
                    return;
                }
                loaded_material.base_texture_loaded = false;
                loaded_material.using_fallback_texture = true;
                loaded_material.definition.constants.flags |= MaterialFlags::FallbackTexture;
            }

            if (loaded_material.base_texture_loaded)
            {
                ++gpu->loaded_texture_count;
            }
            else
            {
                ++gpu->fallback_texture_count;
            }
            gpu->material_constants.push_back(loaded_material.definition.constants);
            gpu->textures.push_back(std::move(gpu_texture));
            cpu_handle.ptr += gpu->srv_descriptor_size;
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

        world_gpu_ = std::move(gpu);
        world_gpu_generation_ = generation;
        log_info("uploaded world mesh '{}' vertices={} triangles={} materials={} textures={} fallback_textures={} batches={}",
            world->name,
            world->mesh.vertices.size(),
            world->mesh.indices.size() / 3,
            world->mesh.materials.size(),
            world_gpu_->loaded_texture_count,
            world_gpu_->fallback_texture_count,
            world_gpu_->batches.size());
    }

    D3D12_VIEWPORT viewport{};
    viewport.TopLeftX = 0.0F;
    viewport.TopLeftY = 0.0F;
    viewport.Width = static_cast<float>(width_);
    viewport.Height = static_cast<float>(height_);
    viewport.MinDepth = 0.0F;
    viewport.MaxDepth = 1.0F;

    D3D12_RECT scissor{};
    scissor.left = 0;
    scissor.top = 0;
    scissor.right = static_cast<LONG>(width_);
    scissor.bottom = static_cast<LONG>(height_);

    const std::array<float, 16> constants = world_to_clip_matrix(world->mesh, &engine_context_->camera, width_, height_);
    ID3D12DescriptorHeap* descriptor_heaps[] = {world_gpu_->srv_heap.Get()};
    command_list_->SetDescriptorHeaps(1, descriptor_heaps);
    command_list_->SetPipelineState(world_pipeline_state_.Get());
    command_list_->SetGraphicsRootSignature(world_root_signature_.Get());
    command_list_->SetGraphicsRoot32BitConstants(0, static_cast<UINT>(constants.size()), constants.data(), 0);
    command_list_->RSSetViewports(1, &viewport);
    command_list_->RSSetScissorRects(1, &scissor);
    command_list_->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    command_list_->IASetVertexBuffers(0, 1, &world_gpu_->vertex_view);
    command_list_->IASetIndexBuffer(&world_gpu_->index_view);
    const D3D12_GPU_DESCRIPTOR_HANDLE gpu_start = world_gpu_->srv_heap->GetGPUDescriptorHandleForHeapStart();
    for (const WorldDrawBatch& batch : world_gpu_->batches)
    {
        if (batch.index_count == 0)
        {
            continue;
        }

        D3D12_GPU_DESCRIPTOR_HANDLE texture_handle = gpu_start;
        texture_handle.ptr += static_cast<UINT64>(batch.material_index) * world_gpu_->srv_descriptor_size;
        command_list_->SetGraphicsRootDescriptorTable(1, texture_handle);
        command_list_->SetGraphicsRoot32BitConstants(2,
            static_cast<UINT>(sizeof(MaterialGpuConstants) / sizeof(std::uint32_t)),
            &world_gpu_->material_constants[batch.material_index],
            0);
        command_list_->DrawIndexedInstanced(batch.index_count, 1, batch.first_index, 0, 0);
    }
}

void Dx12Renderer::shutdown_rml()
{
    rml_console_controller_.reset();
    rml_hud_controller_.reset();
    main_menu_controller_.reset();

    if (rml_initialized_)
    {
        Rml::Shutdown();
        rml_initialized_ = false;
    }

    rml_context_ = nullptr;
    rml_render_interface_.reset();
    rml_system_interface_.reset();
}

void Dx12Renderer::pump_messages()
{
    SDL_Event event{};
    while (SDL_PollEvent(&event))
    {
        if (event.type == SDL_EVENT_QUIT)
        {
            window_closed_ = true;
            return;
        }

        if (event.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED && event.window.windowID == SDL_GetWindowID(window_))
        {
            window_closed_ = true;
            return;
        }

        if (event.type == SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED && event.window.windowID == SDL_GetWindowID(window_))
        {
            if (event.window.data1 > 0 && event.window.data2 > 0)
            {
                width_ = static_cast<std::uint32_t>(event.window.data1);
                height_ = static_cast<std::uint32_t>(event.window.data2);
                resize_pending_ = true;
            }
        }

        if (event.type == SDL_EVENT_KEY_DOWN && event.key.key == SDLK_F8)
        {
            Rml::Debugger::SetVisible(!Rml::Debugger::IsVisible());
            continue;
        }

        if (event.type == SDL_EVENT_KEY_DOWN && rml_console_controller_ != nullptr)
        {
            if (event.key.key == SDLK_GRAVE || event.key.key == SDLK_TILDE)
            {
                rml_console_controller_->toggle();
                continue;
            }

            if (event.key.key == SDLK_ESCAPE && rml_console_controller_->visible())
            {
                rml_console_controller_->hide();
                continue;
            }
        }

        if (event.type == SDL_EVENT_KEY_DOWN && event.key.key == SDLK_ESCAPE && main_menu_controller_ != nullptr &&
            engine_context_ != nullptr && engine_context_->world.current_world() != nullptr)
        {
            main_menu_controller_->set_visible(!main_menu_controller_->visible());
            continue;
        }

        const bool ui_visible = gameplay_ui_visible(rml_console_controller_.get(), main_menu_controller_.get());
        if (engine_context_ != nullptr && !ui_visible && (event.type == SDL_EVENT_KEY_DOWN || event.type == SDL_EVENT_KEY_UP))
        {
            if (set_gameplay_key(engine_context_->input, event.key.key, event.type == SDL_EVENT_KEY_DOWN))
            {
                continue;
            }
        }

        if (engine_context_ != nullptr && !ui_visible && engine_context_->input.mouse_captured && event.type == SDL_EVENT_MOUSE_MOTION)
        {
            engine_context_->input.mouse_delta.x += event.motion.xrel;
            engine_context_->input.mouse_delta.y += event.motion.yrel;
            continue;
        }

        if (rml_context_ != nullptr)
        {
            RmlSDL::InputEventHandler(rml_context_, window_, event);
        }
    }

    if (engine_context_ != nullptr)
    {
        const bool should_capture = engine_context_->world.current_world() != nullptr && !gameplay_ui_visible(rml_console_controller_.get(), main_menu_controller_.get());
        if (!should_capture)
        {
            engine_context_->input.clear_gameplay_buttons();
        }

        if (engine_context_->input.mouse_captured != should_capture)
        {
            if (!SDL_SetWindowRelativeMouseMode(window_, should_capture))
            {
                log_warning("failed to set relative mouse mode: {}", SDL_GetError());
            }
            engine_context_->input.mouse_captured = should_capture;
            engine_context_->input.mouse_delta = {};
        }
    }
}

void Dx12Renderer::wait_for_gpu()
{
    if (!command_queue_ || !fence_ || fence_event_ == nullptr)
    {
        return;
    }

    const std::uint64_t signal_value = fence_value_++;
    command_queue_->Signal(fence_.Get(), signal_value);

    if (fence_->GetCompletedValue() < signal_value)
    {
        fence_->SetEventOnCompletion(signal_value, fence_event_);
        WaitForSingleObject(fence_event_, INFINITE);
    }

    for (std::uint64_t& value : frame_fence_values_)
    {
        value = signal_value;
    }
}

void Dx12Renderer::move_to_next_frame()
{
    const std::uint64_t current_fence_value = fence_value_;
    command_queue_->Signal(fence_.Get(), current_fence_value);
    frame_fence_values_[frame_index_] = current_fence_value;
    ++fence_value_;

    frame_index_ = swap_chain_->GetCurrentBackBufferIndex();

    if (fence_->GetCompletedValue() < frame_fence_values_[frame_index_])
    {
        fence_->SetEventOnCompletion(frame_fence_values_[frame_index_], fence_event_);
        WaitForSingleObject(fence_event_, INFINITE);
    }
}
}
