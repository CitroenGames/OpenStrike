#include "openstrike/renderer/dx12_renderer.hpp"

#include "openstrike/core/log.hpp"
#include "openstrike/engine/engine_context.hpp"
#include "openstrike/engine/sdl_input.hpp"
#include "openstrike/game/game_mode.hpp"
#include "openstrike/material/material_system.hpp"
#include "openstrike/renderer/rml_dx12_render_interface.hpp"
#include "openstrike/renderer/world_material_shader.hpp"
#include "openstrike/source/source_asset_store.hpp"
#include "openstrike/source/source_texture.hpp"
#include "openstrike/ui/main_menu_controller.hpp"
#include "openstrike/ui/rml_console_controller.hpp"
#include "openstrike/ui/rml_hud_controller.hpp"
#include "openstrike/ui/rml_loading_screen_controller.hpp"
#include "openstrike/ui/rml_team_menu_controller.hpp"
#include "openstrike/world/world.hpp"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include <d3d12.h>
#if defined(_DEBUG)
#include <d3d12sdklayers.h>
#endif
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
#include <chrono>
#include <cctype>
#include <cmath>
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <format>
#include <fstream>
#include <future>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
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
constexpr std::size_t kWorldTransformFloatCount = 16;
constexpr std::size_t kWorldShaderFloatCount = 28;
constexpr std::uint32_t kSkyboxCommandListIndex = 0;
constexpr std::uint32_t kWorldCommandListIndex = 1;
constexpr std::uint32_t kForwardPlusTileSize = 16;
constexpr std::uint32_t kForwardPlusDescriptorCount = 3;
constexpr std::uint32_t kWorldLightmapDescriptorCount = 1;
constexpr std::uint32_t kForwardPlusMaxLights = 1024;
constexpr std::uint32_t kForwardPlusMaxLightsPerTile = 64;
constexpr std::uint32_t kShaderDescriptorCapacity = 16384;
constexpr std::uint32_t kRmlDescriptorCount = 128;

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

struct TextureGpuResources
{
    ComPtr<ID3D12Resource> resource;
    ComPtr<ID3D12Resource> upload_buffer;
};

struct ForwardPlusTileGpu
{
    std::uint32_t light_offset = 0;
    std::uint32_t light_count = 0;
    std::uint32_t padding0 = 0;
    std::uint32_t padding1 = 0;
};

struct ForwardPlusLightGpu
{
    float position_radius[4]{};
    float color_intensity[4]{};
};

struct ForwardPlusFrameGpuResources
{
    ComPtr<ID3D12Resource> tile_buffer;
    ComPtr<ID3D12Resource> light_index_buffer;
    ComPtr<ID3D12Resource> light_buffer;
    std::uint32_t tile_capacity = 0;
    std::uint32_t light_index_capacity = 0;
    std::uint32_t light_capacity = 0;
};

struct ClipPoint
{
    float x = 0.0F;
    float y = 0.0F;
    float z = 0.0F;
    float w = 1.0F;
};

struct ForwardPlusTileBounds
{
    std::uint32_t min_x = 0;
    std::uint32_t max_x = 0;
    std::uint32_t min_y = 0;
    std::uint32_t max_y = 0;
};

std::optional<std::filesystem::path> resolve_shader_path(const EngineContext* engine_context, const char* relative_path)
{
    if (relative_path == nullptr || relative_path[0] == '\0')
    {
        return std::nullopt;
    }

    const std::filesystem::path path(relative_path);
    if (engine_context != nullptr)
    {
        if (const std::optional<std::filesystem::path> game_path = engine_context->filesystem.resolve(path, "GAME"))
        {
            return game_path;
        }

        if (const std::optional<std::filesystem::path> any_path = engine_context->filesystem.resolve(path))
        {
            return any_path;
        }
    }

    std::error_code error;
    if (std::filesystem::exists(path, error))
    {
        return path.lexically_normal();
    }

    return std::nullopt;
}

std::string hresult_message(const char* operation, HRESULT result)
{
    return std::string(operation) + " failed with HRESULT 0x" + std::format("{:08X}", static_cast<unsigned>(result));
}

std::wstring widen_debug_name(std::string_view name)
{
    return {name.begin(), name.end()};
}

void set_debug_name(ID3D12Object* object, std::string_view name)
{
    if (object == nullptr || name.empty())
    {
        return;
    }

    const std::wstring wide_name = widen_debug_name(name);
    object->SetName(wide_name.c_str());
}

bool is_device_lost_result(HRESULT result)
{
    return result == DXGI_ERROR_DEVICE_REMOVED || result == DXGI_ERROR_DEVICE_RESET || result == DXGI_ERROR_DEVICE_HUNG ||
        result == DXGI_ERROR_DRIVER_INTERNAL_ERROR;
}

void log_device_removed_reason(ID3D12Device* device, const char* operation, HRESULT result)
{
    if (device == nullptr)
    {
        return;
    }

    const HRESULT reason = device->GetDeviceRemovedReason();
    if (FAILED(reason) || is_device_lost_result(result))
    {
        log_error("{} device removed reason HRESULT 0x{:08X}", operation, static_cast<unsigned>(reason));
    }
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

bool succeeded_with_device(ID3D12Device* device, HRESULT result, const char* operation)
{
    if (SUCCEEDED(result))
    {
        return true;
    }

    log_error("{}", hresult_message(operation, result));
    log_device_removed_reason(device, operation, result);
    return false;
}

#if defined(_DEBUG)
bool enable_dx12_validation_layer()
{
    ComPtr<ID3D12Debug> debug_controller;
    const HRESULT result = D3D12GetDebugInterface(IID_PPV_ARGS(&debug_controller));
    if (FAILED(result))
    {
        log_warning("{}; DX12 validation layer disabled", hresult_message("D3D12GetDebugInterface", result));
        return false;
    }

    debug_controller->EnableDebugLayer();

    ComPtr<ID3D12Debug1> debug_controller1;
    if (SUCCEEDED(debug_controller.As(&debug_controller1)))
    {
        debug_controller1->SetEnableSynchronizedCommandQueueValidation(TRUE);
    }

    log_info("DX12 validation layer enabled");
    return true;
}

void configure_dx12_validation_messages(ID3D12Device* device)
{
    ComPtr<ID3D12InfoQueue> info_queue;
    if (device == nullptr || FAILED(device->QueryInterface(IID_PPV_ARGS(&info_queue))))
    {
        return;
    }

    info_queue->SetBreakOnSeverity(D3D12_MESSAGE_SEVERITY_CORRUPTION, TRUE);
    info_queue->SetBreakOnSeverity(D3D12_MESSAGE_SEVERITY_ERROR, TRUE);
    info_queue->SetBreakOnSeverity(D3D12_MESSAGE_SEVERITY_WARNING, FALSE);
}
#endif

bool wait_for_fence_event(HANDLE event, const char* operation)
{
    const DWORD wait_result = WaitForSingleObject(event, INFINITE);
    if (wait_result == WAIT_OBJECT_0)
    {
        return true;
    }

    log_error("{} failed while waiting for a DX12 fence, WaitForSingleObject={} GetLastError={}",
        operation,
        wait_result,
        GetLastError());
    return false;
}

D3D12_CPU_DESCRIPTOR_HANDLE offset_descriptor(D3D12_CPU_DESCRIPTOR_HANDLE handle, std::uint32_t index, std::uint32_t descriptor_size)
{
    handle.ptr += static_cast<SIZE_T>(index) * descriptor_size;
    return handle;
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

bool upload_static_buffer_to_gpu(
    ID3D12Device* device,
    ID3D12GraphicsCommandList* command_list,
    const void* data,
    std::uint64_t byte_count,
    D3D12_RESOURCE_STATES final_state,
    std::string_view debug_name,
    ComPtr<ID3D12Resource>& output,
    ComPtr<ID3D12Resource>& upload_buffer)
{
    if (device == nullptr || command_list == nullptr || data == nullptr || byte_count == 0)
    {
        return false;
    }

    const auto default_heap = heap_properties(D3D12_HEAP_TYPE_DEFAULT);
    const auto upload_heap = heap_properties(D3D12_HEAP_TYPE_UPLOAD);
    const auto desc = buffer_desc(byte_count);
    if (!succeeded_with_device(device,
            device->CreateCommittedResource(&default_heap,
                D3D12_HEAP_FLAG_NONE,
                &desc,
                D3D12_RESOURCE_STATE_COPY_DEST,
                nullptr,
                IID_PPV_ARGS(&output)),
            "ID3D12Device::CreateCommittedResource static buffer"))
    {
        return false;
    }

    if (!succeeded_with_device(device,
            device->CreateCommittedResource(&upload_heap,
                D3D12_HEAP_FLAG_NONE,
                &desc,
                D3D12_RESOURCE_STATE_GENERIC_READ,
                nullptr,
                IID_PPV_ARGS(&upload_buffer)),
            "ID3D12Device::CreateCommittedResource static buffer upload"))
    {
        return false;
    }

    set_debug_name(output.Get(), debug_name);
    if (!debug_name.empty())
    {
        set_debug_name(upload_buffer.Get(), std::format("{} Upload", debug_name));
    }

    void* mapped = nullptr;
    if (!succeeded_with_device(device, upload_buffer->Map(0, nullptr, &mapped), "ID3D12Resource::Map static buffer upload"))
    {
        return false;
    }
    std::memcpy(mapped, data, static_cast<std::size_t>(byte_count));
    upload_buffer->Unmap(0, nullptr);

    command_list->CopyBufferRegion(output.Get(), 0, upload_buffer.Get(), 0, byte_count);

    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = output.Get();
    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
    barrier.Transition.StateAfter = final_state;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    command_list->ResourceBarrier(1, &barrier);
    return true;
}

bool create_upload_buffer(
    ID3D12Device* device,
    std::uint64_t byte_count,
    std::string_view debug_name,
    ComPtr<ID3D12Resource>& resource)
{
    if (device == nullptr || byte_count == 0)
    {
        return false;
    }

    const auto upload_heap = heap_properties(D3D12_HEAP_TYPE_UPLOAD);
    const auto desc = buffer_desc(byte_count);
    if (!succeeded_with_device(device,
            device->CreateCommittedResource(&upload_heap,
                D3D12_HEAP_FLAG_NONE,
                &desc,
                D3D12_RESOURCE_STATE_GENERIC_READ,
                nullptr,
                IID_PPV_ARGS(&resource)),
            "ID3D12Device::CreateCommittedResource forward+ upload buffer"))
    {
        return false;
    }

    set_debug_name(resource.Get(), debug_name);
    return true;
}

bool upload_buffer_data(ID3D12Resource* resource, const void* data, std::uint64_t byte_count, const char* operation)
{
    if (resource == nullptr || data == nullptr || byte_count == 0)
    {
        return false;
    }

    void* mapped = nullptr;
    D3D12_RANGE read_range{0, 0};
    if (!succeeded(resource->Map(0, &read_range, &mapped), operation))
    {
        return false;
    }
    std::memcpy(mapped, data, static_cast<std::size_t>(byte_count));
    D3D12_RANGE written_range{0, static_cast<SIZE_T>(byte_count)};
    resource->Unmap(0, &written_range);
    return true;
}

void create_structured_buffer_srv(
    ID3D12Device* device,
    ID3D12Resource* resource,
    std::uint32_t element_count,
    std::uint32_t element_stride,
    D3D12_CPU_DESCRIPTOR_HANDLE handle)
{
    D3D12_SHADER_RESOURCE_VIEW_DESC srv_desc{};
    srv_desc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srv_desc.Format = DXGI_FORMAT_UNKNOWN;
    srv_desc.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
    srv_desc.Buffer.FirstElement = 0;
    srv_desc.Buffer.NumElements = std::max<std::uint32_t>(element_count, 1);
    srv_desc.Buffer.StructureByteStride = element_stride;
    srv_desc.Buffer.Flags = D3D12_BUFFER_SRV_FLAG_NONE;
    device->CreateShaderResourceView(resource, &srv_desc, handle);
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
    case SourceTextureFormat::Bc4:
        return DXGI_FORMAT_BC4_UNORM;
    case SourceTextureFormat::Bc5:
        return DXGI_FORMAT_BC5_UNORM;
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
    TextureGpuResources& output,
    std::string_view debug_name)
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
    set_debug_name(output.resource.Get(), debug_name);

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
    if (!debug_name.empty())
    {
        set_debug_name(output.upload_buffer.Get(), std::format("{} Upload", debug_name));
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

bool upload_world_lightmap_to_gpu(
    ID3D12Device* device,
    ID3D12GraphicsCommandList* command_list,
    const WorldLightmapAtlas& atlas,
    D3D12_CPU_DESCRIPTOR_HANDLE cpu_handle,
    TextureGpuResources& output,
    std::string_view debug_name)
{
    if (device == nullptr || command_list == nullptr)
    {
        return false;
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

    constexpr DXGI_FORMAT format = DXGI_FORMAT_R32G32B32A32_FLOAT;
    D3D12_RESOURCE_DESC desc = texture2d_desc(width, height, 1, format);
    const auto default_heap = heap_properties(D3D12_HEAP_TYPE_DEFAULT);
    if (!succeeded(device->CreateCommittedResource(&default_heap,
            D3D12_HEAP_FLAG_NONE,
            &desc,
            D3D12_RESOURCE_STATE_COPY_DEST,
            nullptr,
            IID_PPV_ARGS(&output.resource)),
            "ID3D12Device::CreateCommittedResource world lightmap"))
    {
        return false;
    }
    set_debug_name(output.resource.Get(), debug_name);

    D3D12_PLACED_SUBRESOURCE_FOOTPRINT layout{};
    UINT row_count = 0;
    UINT64 row_size = 0;
    UINT64 upload_bytes = 0;
    device->GetCopyableFootprints(&desc, 0, 1, 0, &layout, &row_count, &row_size, &upload_bytes);
    (void)row_count;
    (void)row_size;

    const auto upload_heap = heap_properties(D3D12_HEAP_TYPE_UPLOAD);
    const auto upload_desc = buffer_desc(upload_bytes);
    if (!succeeded(device->CreateCommittedResource(&upload_heap,
            D3D12_HEAP_FLAG_NONE,
            &upload_desc,
            D3D12_RESOURCE_STATE_GENERIC_READ,
            nullptr,
            IID_PPV_ARGS(&output.upload_buffer)),
            "ID3D12Device::CreateCommittedResource world lightmap upload"))
    {
        return false;
    }
    if (!debug_name.empty())
    {
        set_debug_name(output.upload_buffer.Get(), std::format("{} Upload", debug_name));
    }

    unsigned char* mapped = nullptr;
    if (!succeeded(output.upload_buffer->Map(0, nullptr, reinterpret_cast<void**>(&mapped)), "ID3D12Resource::Map world lightmap upload"))
    {
        return false;
    }

    const std::uint64_t source_row_bytes = static_cast<std::uint64_t>(width) * 4U * sizeof(float);
    unsigned char* dst = mapped + layout.Offset;
    const unsigned char* src = reinterpret_cast<const unsigned char*>(pixels);
    for (std::uint32_t row = 0; row < height; ++row)
    {
        std::memcpy(dst + (static_cast<std::uint64_t>(row) * layout.Footprint.RowPitch),
            src + (static_cast<std::uint64_t>(row) * source_row_bytes),
            static_cast<std::size_t>(source_row_bytes));
    }
    output.upload_buffer->Unmap(0, nullptr);

    D3D12_TEXTURE_COPY_LOCATION src_location{};
    src_location.pResource = output.upload_buffer.Get();
    src_location.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    src_location.PlacedFootprint = layout;

    D3D12_TEXTURE_COPY_LOCATION dst_location{};
    dst_location.pResource = output.resource.Get();
    dst_location.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    dst_location.SubresourceIndex = 0;
    command_list->CopyTextureRegion(&dst_location, 0, 0, 0, &src_location, nullptr);

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
    srv_desc.Texture2D.MipLevels = 1;
    srv_desc.Texture2D.ResourceMinLODClamp = 0.0F;
    device->CreateShaderResourceView(output.resource.Get(), &srv_desc, cpu_handle);
    return true;
}

ComPtr<ID3DBlob> read_shader_blob(const std::filesystem::path& path, const char* debug_name)
{
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file)
    {
        log_warning("failed to open compiled shader '{}': {}", debug_name, path.string());
        return {};
    }

    const std::streamoff file_size = static_cast<std::streamoff>(file.tellg());
    if (file_size <= 0)
    {
        log_warning("compiled shader '{}' is empty: {}", debug_name, path.string());
        return {};
    }

    ComPtr<ID3DBlob> shader;
    if (!succeeded(D3DCreateBlob(static_cast<SIZE_T>(file_size), &shader), "D3DCreateBlob shader bytecode"))
    {
        return {};
    }

    file.seekg(0, std::ios::beg);
    file.read(static_cast<char*>(shader->GetBufferPointer()), static_cast<std::streamsize>(file_size));
    if (!file)
    {
        log_warning("failed to read compiled shader '{}': {}", debug_name, path.string());
        return {};
    }

    return shader;
}

ComPtr<ID3DBlob> compile_shader_file(const std::filesystem::path& source_path, const Dx12ShaderFile& shader_file)
{
    UINT flags = D3DCOMPILE_ENABLE_STRICTNESS;
#if defined(_DEBUG)
    flags |= D3DCOMPILE_DEBUG | D3DCOMPILE_SKIP_OPTIMIZATION;
#endif

    ComPtr<ID3DBlob> shader;
    ComPtr<ID3DBlob> errors;
    const HRESULT result = D3DCompileFromFile(
        source_path.c_str(),
        nullptr,
        D3D_COMPILE_STANDARD_FILE_INCLUDE,
        shader_file.entry_point,
        shader_file.target,
        flags,
        0,
        &shader,
        &errors);
    if (FAILED(result))
    {
        if (errors)
        {
            log_error("renderer shader compile error in '{}': {}", shader_file.debug_name, static_cast<const char*>(errors->GetBufferPointer()));
        }
        else
        {
            log_error("{}", hresult_message("D3DCompile", result));
        }
        return {};
    }

    return shader;
}

ComPtr<ID3DBlob> load_shader_bytecode(const EngineContext* engine_context, const Dx12ShaderFile& shader_file)
{
    const std::optional<std::filesystem::path> source_path = resolve_shader_path(engine_context, shader_file.source_path);
    if (const std::optional<std::filesystem::path> compiled_path = resolve_shader_path(engine_context, shader_file.compiled_path))
    {
        bool compiled_is_current = true;
        if (source_path)
        {
            std::error_code compiled_error;
            std::error_code source_error;
            const auto compiled_time = std::filesystem::last_write_time(*compiled_path, compiled_error);
            const auto source_time = std::filesystem::last_write_time(*source_path, source_error);
            compiled_is_current = !compiled_error && !source_error && compiled_time >= source_time;
        }

        if (compiled_is_current)
        {
            if (ComPtr<ID3DBlob> shader = read_shader_blob(*compiled_path, shader_file.debug_name))
            {
                return shader;
            }
        }
    }

    if (!source_path)
    {
        log_error("missing DX12 shader '{}' (expected '{}' or '{}')",
            shader_file.debug_name,
            shader_file.compiled_path,
            shader_file.source_path);
        return {};
    }

    return compile_shader_file(*source_path, shader_file);
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

ClipPoint transform_world_to_clip(Vec3 point, const std::array<float, 16>& matrix)
{
    return {
        (point.x * matrix[0]) + (point.y * matrix[4]) + (point.z * matrix[8]) + matrix[12],
        (point.x * matrix[1]) + (point.y * matrix[5]) + (point.z * matrix[9]) + matrix[13],
        (point.x * matrix[2]) + (point.y * matrix[6]) + (point.z * matrix[10]) + matrix[14],
        (point.x * matrix[3]) + (point.y * matrix[7]) + (point.z * matrix[11]) + matrix[15],
    };
}

bool project_world_to_screen(
    Vec3 point,
    const std::array<float, 16>& matrix,
    std::uint32_t width,
    std::uint32_t height,
    float& screen_x,
    float& screen_y)
{
    const ClipPoint clip = transform_world_to_clip(point, matrix);
    if (clip.w <= 0.001F)
    {
        return false;
    }

    const float ndc_x = clip.x / clip.w;
    const float ndc_y = clip.y / clip.w;
    screen_x = (ndc_x * 0.5F + 0.5F) * static_cast<float>(std::max<std::uint32_t>(width, 1));
    screen_y = (0.5F - (ndc_y * 0.5F)) * static_cast<float>(std::max<std::uint32_t>(height, 1));
    return std::isfinite(screen_x) && std::isfinite(screen_y);
}

ForwardPlusTileBounds full_screen_tile_bounds(std::uint32_t tile_count_x, std::uint32_t tile_count_y)
{
    return {
        0,
        tile_count_x > 0 ? tile_count_x - 1U : 0U,
        0,
        tile_count_y > 0 ? tile_count_y - 1U : 0U,
    };
}

std::optional<ForwardPlusTileBounds> light_tile_bounds(
    const WorldLight& light,
    const std::array<float, 16>& matrix,
    bool perspective_camera,
    std::uint32_t width,
    std::uint32_t height,
    std::uint32_t tile_count_x,
    std::uint32_t tile_count_y)
{
    if (width == 0 || height == 0 || tile_count_x == 0 || tile_count_y == 0 || light.radius <= 0.0F)
    {
        return std::nullopt;
    }

    const ClipPoint center_clip = transform_world_to_clip(light.position, matrix);
    if (perspective_camera && center_clip.w <= -light.radius)
    {
        return std::nullopt;
    }
    if (perspective_camera && center_clip.w <= std::max(light.radius, 1.0F))
    {
        return full_screen_tile_bounds(tile_count_x, tile_count_y);
    }

    const std::array<Vec3, 7> samples{{
        light.position,
        {light.position.x - light.radius, light.position.y, light.position.z},
        {light.position.x + light.radius, light.position.y, light.position.z},
        {light.position.x, light.position.y - light.radius, light.position.z},
        {light.position.x, light.position.y + light.radius, light.position.z},
        {light.position.x, light.position.y, light.position.z - light.radius},
        {light.position.x, light.position.y, light.position.z + light.radius},
    }};

    float min_x = std::numeric_limits<float>::max();
    float min_y = std::numeric_limits<float>::max();
    float max_x = -std::numeric_limits<float>::max();
    float max_y = -std::numeric_limits<float>::max();
    bool any_projected = false;
    for (const Vec3 sample : samples)
    {
        float screen_x = 0.0F;
        float screen_y = 0.0F;
        if (!project_world_to_screen(sample, matrix, width, height, screen_x, screen_y))
        {
            continue;
        }

        any_projected = true;
        min_x = std::min(min_x, screen_x);
        min_y = std::min(min_y, screen_y);
        max_x = std::max(max_x, screen_x);
        max_y = std::max(max_y, screen_y);
    }

    if (!any_projected || max_x < 0.0F || max_y < 0.0F || min_x >= static_cast<float>(width) || min_y >= static_cast<float>(height))
    {
        return std::nullopt;
    }

    min_x = std::clamp(min_x, 0.0F, static_cast<float>(width - 1U));
    max_x = std::clamp(max_x, 0.0F, static_cast<float>(width - 1U));
    min_y = std::clamp(min_y, 0.0F, static_cast<float>(height - 1U));
    max_y = std::clamp(max_y, 0.0F, static_cast<float>(height - 1U));

    return ForwardPlusTileBounds{
        static_cast<std::uint32_t>(std::floor(min_x / static_cast<float>(kForwardPlusTileSize))),
        std::min(tile_count_x - 1U, static_cast<std::uint32_t>(std::floor(max_x / static_cast<float>(kForwardPlusTileSize)))),
        static_cast<std::uint32_t>(std::floor(min_y / static_cast<float>(kForwardPlusTileSize))),
        std::min(tile_count_y - 1U, static_cast<std::uint32_t>(std::floor(max_y / static_cast<float>(kForwardPlusTileSize)))),
    };
}

ForwardPlusLightGpu forward_plus_light_gpu(const WorldLight& light)
{
    ForwardPlusLightGpu gpu{};
    gpu.position_radius[0] = light.position.x;
    gpu.position_radius[1] = light.position.y;
    gpu.position_radius[2] = light.position.z;
    gpu.position_radius[3] = std::max(light.radius, 1.0F);
    gpu.color_intensity[0] = light.color[0];
    gpu.color_intensity[1] = light.color[1];
    gpu.color_intensity[2] = light.color[2];
    gpu.color_intensity[3] = light.intensity;
    return gpu;
}

std::array<float, kWorldShaderFloatCount> world_shader_constants(
    const WorldMesh& mesh,
    const CameraState* camera,
    std::uint32_t width,
    std::uint32_t height)
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
    constants[kWorldTransformFloatCount + 8] = static_cast<float>(kForwardPlusTileSize);
    constants[kWorldTransformFloatCount + 9] = static_cast<float>((width + kForwardPlusTileSize - 1U) / kForwardPlusTileSize);
    constants[kWorldTransformFloatCount + 10] = static_cast<float>((height + kForwardPlusTileSize - 1U) / kForwardPlusTileSize);
    constants[kWorldTransformFloatCount + 11] = 0.0F;
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

bool gameplay_ui_visible(
    const RmlConsoleController* console,
    const MainMenuController* main_menu,
    const RmlLoadingScreenController* loading_screen,
    const RmlTeamMenuController* team_menu)
{
    return (console != nullptr && console->visible()) || (main_menu != nullptr && main_menu->visible()) ||
           (loading_screen != nullptr && loading_screen->visible()) || (team_menu != nullptr && team_menu->visible());
}

double elapsed_us(std::chrono::steady_clock::time_point begin, std::chrono::steady_clock::time_point end)
{
    return std::chrono::duration<double, std::micro>(end - begin).count();
}

}

struct ShaderDescriptorRange
{
    D3D12_CPU_DESCRIPTOR_HANDLE cpu_start{};
    D3D12_GPU_DESCRIPTOR_HANDLE gpu_start{};
    std::uint32_t first_descriptor = 0;
    std::uint32_t descriptor_count = 0;
};

struct Dx12Renderer::WorldGpuResources
{
    Microsoft::WRL::ComPtr<ID3D12Resource> vertex_buffer;
    Microsoft::WRL::ComPtr<ID3D12Resource> index_buffer;
    Microsoft::WRL::ComPtr<ID3D12Resource> vertex_upload_buffer;
    Microsoft::WRL::ComPtr<ID3D12Resource> index_upload_buffer;
    ShaderDescriptorRange srv_range;
    D3D12_VERTEX_BUFFER_VIEW vertex_view{};
    D3D12_INDEX_BUFFER_VIEW index_view{};
    std::vector<TextureGpuResources> textures;
    TextureGpuResources lightmap_texture;
    std::vector<MaterialGpuConstants> material_constants;
    std::vector<WorldDrawBatch> batches;
    std::array<ForwardPlusFrameGpuResources, kFrameCount> forward_plus_frames;
    std::uint32_t material_descriptor_count = 0;
    std::uint32_t index_count = 0;
    std::uint32_t loaded_texture_count = 0;
    std::uint32_t fallback_texture_count = 0;
    std::uint32_t forward_plus_light_count = 0;
    std::vector<ForwardPlusLightGpu> forward_plus_lights;
    std::vector<ForwardPlusTileGpu> forward_plus_tiles;
    std::vector<std::uint32_t> forward_plus_tile_counts;
    std::vector<std::uint32_t> forward_plus_tile_write_offsets;
    std::vector<std::uint32_t> forward_plus_light_indices;
    std::vector<ForwardPlusTileBounds> forward_plus_light_bounds;
    std::vector<std::uint8_t> forward_plus_light_visible;
};

struct Dx12Renderer::SkyboxGpuResources
{
    std::string sky_name;
    Microsoft::WRL::ComPtr<ID3D12Resource> vertex_buffer;
    Microsoft::WRL::ComPtr<ID3D12Resource> index_buffer;
    Microsoft::WRL::ComPtr<ID3D12Resource> vertex_upload_buffer;
    Microsoft::WRL::ComPtr<ID3D12Resource> index_upload_buffer;
    ShaderDescriptorRange srv_range;
    D3D12_VERTEX_BUFFER_VIEW vertex_view{};
    D3D12_INDEX_BUFFER_VIEW index_view{};
    std::array<TextureGpuResources, kSkyboxFaceCount> textures;
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
    dx12_profile_ = config.dx12_profile;
    async_scene_recording_ = config.dx12_async_recording;
    if (dx12_profile_)
    {
        const std::filesystem::path profile_path = config.dx12_profile_path.empty() ? std::filesystem::path("dx12_profile.csv") : config.dx12_profile_path;
        dx12_profile_stream_.open(profile_path, std::ios::out | std::ios::trunc);
        if (!dx12_profile_stream_)
        {
            log_warning("failed to open DX12 profile output '{}'; profiling disabled", profile_path.string());
            dx12_profile_ = false;
        }
        else
        {
            dx12_profile_stream_
                << "frame,total_us,pump_us,ui_us,setup_us,resource_us,scene_record_us,rml_us,submit_present_us,"
                   "skybox_ready,world_ready,async_recording,command_lists,world_batches,rml_draws,rml_descriptor_heap_sets,"
                   "rml_upload_resource_creations,rml_vertex_upload_bytes,rml_index_upload_bytes\n";
        }
    }

    if (!create_window(config) || !create_device_objects(config) || !create_multicore_command_objects() || !create_world_pipeline() ||
        !create_skybox_pipeline() || !create_render_targets() || !create_depth_buffer() || !initialize_rml(config))
    {
        shutdown();
        return false;
    }

    initialized_ = true;
    log_info("dx12 renderer initialized {}x{} vsync={} async_recording={}",
        width_,
        height_,
        vsync_ ? "on" : "off",
        async_scene_recording_ ? "on" : "off");
    return true;
}

void Dx12Renderer::render(const FrameContext& context)
{
    if (!initialized_ || window_closed_)
    {
        return;
    }

    const auto total_start = std::chrono::steady_clock::now();
    double pump_us = 0.0;
    double ui_us = 0.0;
    double setup_us = 0.0;
    double resource_us = 0.0;
    double scene_record_us = 0.0;
    double rml_us = 0.0;
    double submit_present_us = 0.0;

    auto phase_start = total_start;
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
    pump_us = elapsed_us(phase_start, std::chrono::steady_clock::now());

    phase_start = std::chrono::steady_clock::now();
    if (rml_console_controller_ != nullptr)
    {
        rml_console_controller_->update();
    }

    if (rml_loading_screen_controller_ != nullptr && engine_context_ != nullptr)
    {
        rml_loading_screen_controller_->update(engine_context_->loading_screen);
    }

    sync_main_menu_visibility();
    const bool loading_screen_visible = rml_loading_screen_controller_ != nullptr && rml_loading_screen_controller_->visible();
    const bool main_menu_visible = main_menu_controller_ != nullptr && main_menu_controller_->visible();
    if (rml_team_menu_controller_ != nullptr)
    {
        rml_team_menu_controller_->update(main_menu_visible, loading_screen_visible);
    }
    if (rml_hud_controller_ != nullptr)
    {
        const bool team_menu_visible = rml_team_menu_controller_ != nullptr && rml_team_menu_controller_->visible();
        rml_hud_controller_->update(main_menu_visible || loading_screen_visible || team_menu_visible);
    }

    if (rml_context_ != nullptr)
    {
        rml_context_->Update();
    }
    ui_us = elapsed_us(phase_start, std::chrono::steady_clock::now());

    phase_start = std::chrono::steady_clock::now();
    const std::uint32_t frame = frame_index_;
    ID3D12CommandAllocator* allocator = command_allocators_[frame].Get();
    if (!succeeded_with_device(device_.Get(), allocator->Reset(), "ID3D12CommandAllocator::Reset frame"))
    {
        window_closed_ = true;
        return;
    }
    if (!succeeded_with_device(device_.Get(), command_list_->Reset(allocator, nullptr), "ID3D12GraphicsCommandList::Reset frame"))
    {
        window_closed_ = true;
        return;
    }

    D3D12_RESOURCE_BARRIER begin_barrier{};
    begin_barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    begin_barrier.Transition.pResource = render_targets_[frame].Get();
    begin_barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
    begin_barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
    begin_barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    command_list_->ResourceBarrier(1, &begin_barrier);

    const float pulse = static_cast<float>((context.frame_index % 120) / 120.0);
    const std::array<float, 4> clear_color{0.03F, 0.05F + (0.05F * pulse), 0.09F + (0.15F * pulse), 1.0F};

    D3D12_CPU_DESCRIPTOR_HANDLE rtv_handle = offset_descriptor(rtv_heap_->GetCPUDescriptorHandleForHeapStart(), frame, rtv_descriptor_size_);
    D3D12_CPU_DESCRIPTOR_HANDLE dsv_handle = dsv_heap_->GetCPUDescriptorHandleForHeapStart();
    command_list_->OMSetRenderTargets(1, &rtv_handle, FALSE, &dsv_handle);
    command_list_->ClearRenderTargetView(rtv_handle, clear_color.data(), 0, nullptr);
    command_list_->ClearDepthStencilView(dsv_handle, D3D12_CLEAR_FLAG_DEPTH, 1.0F, 0, 0, nullptr);
    setup_us = elapsed_us(phase_start, std::chrono::steady_clock::now());

    phase_start = std::chrono::steady_clock::now();
    const LoadedWorld* active_world = engine_context_ != nullptr ? engine_context_->world.current_world() : nullptr;
    bool skybox_ready = false;
    if (active_world != nullptr)
    {
        skybox_ready = ensure_skybox_gpu_resources(*active_world);
    }
    else
    {
        skybox_gpu_.reset();
        skybox_gpu_generation_ = engine_context_ != nullptr ? engine_context_->world.generation() : 0;
    }

    bool world_ready = ensure_world_gpu_resources();
    if (world_ready && active_world != nullptr)
    {
        world_ready = upload_forward_plus_resources(*active_world);
    }

    if (!succeeded_with_device(device_.Get(), command_list_->Close(), "ID3D12GraphicsCommandList::Close setup"))
    {
        window_closed_ = true;
        return;
    }
    resource_us = elapsed_us(phase_start, std::chrono::steady_clock::now());

    phase_start = std::chrono::steady_clock::now();
    if (skybox_ready)
    {
        ID3D12CommandAllocator* scene_allocator = scene_command_allocators_[frame][kSkyboxCommandListIndex].Get();
        ID3D12GraphicsCommandList* scene_list = scene_command_lists_[kSkyboxCommandListIndex].Get();
        if (!succeeded_with_device(device_.Get(), scene_allocator->Reset(), "ID3D12CommandAllocator::Reset skybox scene") ||
            !succeeded_with_device(device_.Get(), scene_list->Reset(scene_allocator, nullptr), "ID3D12GraphicsCommandList::Reset skybox scene"))
        {
            window_closed_ = true;
            return;
        }
    }

    if (world_ready)
    {
        ID3D12CommandAllocator* scene_allocator = scene_command_allocators_[frame][kWorldCommandListIndex].Get();
        ID3D12GraphicsCommandList* scene_list = scene_command_lists_[kWorldCommandListIndex].Get();
        if (!succeeded_with_device(device_.Get(), scene_allocator->Reset(), "ID3D12CommandAllocator::Reset world scene") ||
            !succeeded_with_device(device_.Get(), scene_list->Reset(scene_allocator, nullptr), "ID3D12GraphicsCommandList::Reset world scene"))
        {
            window_closed_ = true;
            return;
        }
    }

    auto close_scene_list = [this](ID3D12GraphicsCommandList* list, const char* operation) {
        return succeeded_with_device(device_.Get(), list->Close(), operation);
    };

    bool scene_recording_ok = true;
    const bool used_async_recording = skybox_ready && world_ready && async_scene_recording_;
    if (used_async_recording)
    {
        std::future<bool> skybox_recording = std::async(std::launch::async, [this, active_world, close_scene_list] {
            ID3D12GraphicsCommandList* list = scene_command_lists_[kSkyboxCommandListIndex].Get();
            return record_skybox(list, *active_world) && close_scene_list(list, "ID3D12GraphicsCommandList::Close skybox scene");
        });

        ID3D12GraphicsCommandList* world_list = scene_command_lists_[kWorldCommandListIndex].Get();
        scene_recording_ok = record_world(world_list) && close_scene_list(world_list, "ID3D12GraphicsCommandList::Close world scene");
        scene_recording_ok = skybox_recording.get() && scene_recording_ok;
    }
    else if (skybox_ready && world_ready)
    {
        ID3D12GraphicsCommandList* skybox_list = scene_command_lists_[kSkyboxCommandListIndex].Get();
        ID3D12GraphicsCommandList* world_list = scene_command_lists_[kWorldCommandListIndex].Get();
        scene_recording_ok =
            record_skybox(skybox_list, *active_world) && close_scene_list(skybox_list, "ID3D12GraphicsCommandList::Close skybox scene") &&
            record_world(world_list) && close_scene_list(world_list, "ID3D12GraphicsCommandList::Close world scene");
    }
    else if (skybox_ready)
    {
        ID3D12GraphicsCommandList* list = scene_command_lists_[kSkyboxCommandListIndex].Get();
        scene_recording_ok = record_skybox(list, *active_world) && close_scene_list(list, "ID3D12GraphicsCommandList::Close skybox scene");
    }
    else if (world_ready)
    {
        ID3D12GraphicsCommandList* list = scene_command_lists_[kWorldCommandListIndex].Get();
        scene_recording_ok = record_world(list) && close_scene_list(list, "ID3D12GraphicsCommandList::Close world scene");
    }

    if (!scene_recording_ok)
    {
        window_closed_ = true;
        return;
    }
    scene_record_us = elapsed_us(phase_start, std::chrono::steady_clock::now());

    phase_start = std::chrono::steady_clock::now();
    RmlDx12FrameStats rml_stats{};
    ID3D12CommandAllocator* post_allocator = post_command_allocators_[frame].Get();
    if (!succeeded_with_device(device_.Get(), post_allocator->Reset(), "ID3D12CommandAllocator::Reset post") ||
        !succeeded_with_device(device_.Get(), post_command_list_->Reset(post_allocator, nullptr), "ID3D12GraphicsCommandList::Reset post"))
    {
        window_closed_ = true;
        return;
    }

    if (rml_context_ != nullptr && rml_render_interface_)
    {
        post_command_list_->OMSetRenderTargets(1, &rtv_handle, FALSE, nullptr);
        rml_render_interface_->begin_frame(post_command_list_.Get(), frame, width_, height_);
        rml_context_->Render();
        rml_stats = rml_render_interface_->frame_stats();
        rml_render_interface_->end_frame();
    }

    D3D12_RESOURCE_BARRIER end_barrier = begin_barrier;
    end_barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
    end_barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
    post_command_list_->ResourceBarrier(1, &end_barrier);

    if (!succeeded_with_device(device_.Get(), post_command_list_->Close(), "ID3D12GraphicsCommandList::Close post"))
    {
        window_closed_ = true;
        return;
    }
    rml_us = elapsed_us(phase_start, std::chrono::steady_clock::now());

    phase_start = std::chrono::steady_clock::now();
    std::array<ID3D12CommandList*, kSceneCommandListCount + 2> lists{};
    std::uint32_t list_count = 0;
    lists[list_count++] = command_list_.Get();
    if (skybox_ready)
    {
        lists[list_count++] = scene_command_lists_[kSkyboxCommandListIndex].Get();
    }
    if (world_ready)
    {
        lists[list_count++] = scene_command_lists_[kWorldCommandListIndex].Get();
    }
    lists[list_count++] = post_command_list_.Get();
    command_queue_->ExecuteCommandLists(list_count, lists.data());

    const UINT present_sync = vsync_ ? 1U : 0U;
    const UINT present_flags = (!vsync_ && allow_tearing_) ? DXGI_PRESENT_ALLOW_TEARING : 0U;
    const HRESULT present_result = swap_chain_->Present(present_sync, present_flags);
    if (!succeeded_with_device(device_.Get(), present_result, "IDXGISwapChain::Present"))
    {
        window_closed_ = true;
        return;
    }

    if (!move_to_next_frame())
    {
        window_closed_ = true;
        return;
    }
    submit_present_us = elapsed_us(phase_start, std::chrono::steady_clock::now());

    if (dx12_profile_ && dx12_profile_stream_)
    {
        const double total_us = elapsed_us(total_start, std::chrono::steady_clock::now());
        const std::uint32_t world_batches = world_gpu_ != nullptr ? static_cast<std::uint32_t>(world_gpu_->batches.size()) : 0U;
        dx12_profile_stream_ << context.frame_index << ','
                             << total_us << ','
                             << pump_us << ','
                             << ui_us << ','
                             << setup_us << ','
                             << resource_us << ','
                             << scene_record_us << ','
                             << rml_us << ','
                             << submit_present_us << ','
                             << (skybox_ready ? 1 : 0) << ','
                             << (world_ready ? 1 : 0) << ','
                             << (used_async_recording ? 1 : 0) << ','
                             << list_count << ','
                             << world_batches << ','
                             << rml_stats.draw_calls << ','
                             << rml_stats.descriptor_heap_sets << ','
                             << rml_stats.upload_resource_creations << ','
                             << rml_stats.vertex_upload_bytes << ','
                             << rml_stats.index_upload_bytes << '\n';
        if ((context.frame_index % 120) == 119)
        {
            dx12_profile_stream_.flush();
        }
    }
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
    scene_command_lists_ = {};
    scene_command_allocators_ = {};
    post_command_list_.Reset();
    post_command_allocators_ = {};
    shader_descriptor_heap_.Reset();
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
    if (dx12_profile_stream_.is_open())
    {
        dx12_profile_stream_.flush();
        dx12_profile_stream_.close();
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
    shader_descriptor_size_ = 0;
    shader_descriptor_capacity_ = 0;
    shader_descriptor_next_ = 0;
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
    const bool dx12_validation_enabled = enable_dx12_validation_layer();
#endif

    UINT factory_flags = 0;
#if defined(_DEBUG)
    if (dx12_validation_enabled)
    {
        factory_flags |= DXGI_CREATE_FACTORY_DEBUG;
    }
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
    set_debug_name(device_.Get(), "OpenStrike D3D12 Device");
#if defined(_DEBUG)
    if (dx12_validation_enabled)
    {
        configure_dx12_validation_messages(device_.Get());
    }
#endif

    D3D12_COMMAND_QUEUE_DESC queue_desc{};
    queue_desc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    queue_desc.Flags = D3D12_COMMAND_QUEUE_FLAG_NONE;
    if (!succeeded(device_->CreateCommandQueue(&queue_desc, IID_PPV_ARGS(&command_queue_)), "ID3D12Device::CreateCommandQueue"))
    {
        return false;
    }
    set_debug_name(command_queue_.Get(), "OpenStrike Graphics Queue");

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
    set_debug_name(rtv_heap_.Get(), "OpenStrike Back Buffer RTV Heap");

    rtv_descriptor_size_ = device_->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    dsv_descriptor_size_ = device_->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_DSV);
    if (!create_shader_descriptor_heap())
    {
        return false;
    }

    for (std::uint32_t frame = 0; frame < kFrameCount; ++frame)
    {
        if (!succeeded(device_->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&command_allocators_[frame])),
                "ID3D12Device::CreateCommandAllocator"))
        {
            return false;
        }
        set_debug_name(command_allocators_[frame].Get(), std::format("OpenStrike Frame {} Command Allocator", frame));
    }

    if (!succeeded(device_->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, command_allocators_[frame_index_].Get(), nullptr,
            IID_PPV_ARGS(&command_list_)), "ID3D12Device::CreateCommandList"))
    {
        return false;
    }
    set_debug_name(command_list_.Get(), "OpenStrike Main Command List");
    if (!succeeded_with_device(device_.Get(), command_list_->Close(), "ID3D12GraphicsCommandList::Close initial"))
    {
        return false;
    }

    if (!succeeded(device_->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence_)), "ID3D12Device::CreateFence"))
    {
        return false;
    }
    set_debug_name(fence_.Get(), "OpenStrike Frame Fence");

    fence_value_ = 1;
    fence_event_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (fence_event_ == nullptr)
    {
        log_error("CreateEventW failed for DX12 fence");
        return false;
    }

    return true;
}

bool Dx12Renderer::create_shader_descriptor_heap()
{
    D3D12_DESCRIPTOR_HEAP_DESC desc{};
    desc.NumDescriptors = kShaderDescriptorCapacity;
    desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    if (!succeeded(device_->CreateDescriptorHeap(&desc, IID_PPV_ARGS(&shader_descriptor_heap_)),
            "ID3D12Device::CreateDescriptorHeap shared shader descriptors"))
    {
        return false;
    }

    set_debug_name(shader_descriptor_heap_.Get(), "OpenStrike Shared Shader Descriptor Heap");
    shader_descriptor_size_ = device_->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    shader_descriptor_capacity_ = kShaderDescriptorCapacity;
    shader_descriptor_next_ = 0;
    return true;
}

bool Dx12Renderer::allocate_shader_descriptors(std::uint32_t count, ShaderDescriptorRange& range)
{
    if (shader_descriptor_heap_ == nullptr || count == 0)
    {
        return false;
    }

    if (count > shader_descriptor_capacity_ || shader_descriptor_next_ > shader_descriptor_capacity_ - count)
    {
        log_error("shared DX12 shader descriptor heap exhausted; requested={} used={} capacity={}",
            count,
            shader_descriptor_next_,
            shader_descriptor_capacity_);
        return false;
    }

    range.first_descriptor = shader_descriptor_next_;
    range.descriptor_count = count;
    range.cpu_start = shader_descriptor_heap_->GetCPUDescriptorHandleForHeapStart();
    range.cpu_start.ptr += static_cast<SIZE_T>(range.first_descriptor) * shader_descriptor_size_;
    range.gpu_start = shader_descriptor_heap_->GetGPUDescriptorHandleForHeapStart();
    range.gpu_start.ptr += static_cast<UINT64>(range.first_descriptor) * shader_descriptor_size_;
    shader_descriptor_next_ += count;
    return true;
}

bool Dx12Renderer::create_multicore_command_objects()
{
    if (!device_)
    {
        return false;
    }

    for (std::uint32_t frame = 0; frame < kFrameCount; ++frame)
    {
        for (std::uint32_t list_index = 0; list_index < kSceneCommandListCount; ++list_index)
        {
            if (!succeeded(device_->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&scene_command_allocators_[frame][list_index])),
                    "ID3D12Device::CreateCommandAllocator scene"))
            {
                return false;
            }
            set_debug_name(scene_command_allocators_[frame][list_index].Get(),
                std::format("OpenStrike Frame {} Scene Command Allocator {}", frame, list_index));
        }

        if (!succeeded(device_->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&post_command_allocators_[frame])),
                "ID3D12Device::CreateCommandAllocator post"))
        {
            return false;
        }
        set_debug_name(post_command_allocators_[frame].Get(), std::format("OpenStrike Frame {} Post Command Allocator", frame));
    }

    for (std::uint32_t list_index = 0; list_index < kSceneCommandListCount; ++list_index)
    {
        if (!succeeded(device_->CreateCommandList(0,
                D3D12_COMMAND_LIST_TYPE_DIRECT,
                scene_command_allocators_[frame_index_][list_index].Get(),
                nullptr,
                IID_PPV_ARGS(&scene_command_lists_[list_index])),
                "ID3D12Device::CreateCommandList scene"))
        {
            return false;
        }

        set_debug_name(scene_command_lists_[list_index].Get(), std::format("OpenStrike Scene Command List {}", list_index));
        if (!succeeded_with_device(device_.Get(),
                scene_command_lists_[list_index]->Close(),
                "ID3D12GraphicsCommandList::Close scene initial"))
        {
            return false;
        }
    }

    if (!succeeded(device_->CreateCommandList(0,
            D3D12_COMMAND_LIST_TYPE_DIRECT,
            post_command_allocators_[frame_index_].Get(),
            nullptr,
            IID_PPV_ARGS(&post_command_list_)),
            "ID3D12Device::CreateCommandList post"))
    {
        return false;
    }

    set_debug_name(post_command_list_.Get(), "OpenStrike Post Command List");
    if (!succeeded_with_device(device_.Get(), post_command_list_->Close(), "ID3D12GraphicsCommandList::Close post initial"))
    {
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
        set_debug_name(render_targets_[frame].Get(), std::format("OpenStrike Back Buffer {}", frame));

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
        set_debug_name(dsv_heap_.Get(), "OpenStrike Depth DSV Heap");
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
    set_debug_name(depth_buffer_.Get(), "OpenStrike Depth Buffer");

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

    D3D12_DESCRIPTOR_RANGE lightmap_range{};
    lightmap_range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    lightmap_range.NumDescriptors = kWorldLightmapDescriptorCount;
    lightmap_range.BaseShaderRegister = 1;
    lightmap_range.RegisterSpace = 0;
    lightmap_range.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

    D3D12_DESCRIPTOR_RANGE forward_plus_range{};
    forward_plus_range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    forward_plus_range.NumDescriptors = kForwardPlusDescriptorCount;
    forward_plus_range.BaseShaderRegister = 2;
    forward_plus_range.RegisterSpace = 0;
    forward_plus_range.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

    std::array<D3D12_ROOT_PARAMETER, 5> root_parameters{};
    root_parameters[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    root_parameters[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    root_parameters[0].Constants.ShaderRegister = 0;
    root_parameters[0].Constants.RegisterSpace = 0;
    root_parameters[0].Constants.Num32BitValues = static_cast<UINT>(kWorldShaderFloatCount);

    root_parameters[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    root_parameters[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    root_parameters[1].DescriptorTable.NumDescriptorRanges = 1;
    root_parameters[1].DescriptorTable.pDescriptorRanges = &texture_range;

    root_parameters[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    root_parameters[2].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    root_parameters[2].Constants.ShaderRegister = 1;
    root_parameters[2].Constants.RegisterSpace = 0;
    root_parameters[2].Constants.Num32BitValues = static_cast<UINT>(sizeof(MaterialGpuConstants) / sizeof(std::uint32_t));

    root_parameters[3].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    root_parameters[3].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    root_parameters[3].DescriptorTable.NumDescriptorRanges = 1;
    root_parameters[3].DescriptorTable.pDescriptorRanges = &lightmap_range;

    root_parameters[4].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    root_parameters[4].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    root_parameters[4].DescriptorTable.NumDescriptorRanges = 1;
    root_parameters[4].DescriptorTable.pDescriptorRanges = &forward_plus_range;

    std::array<D3D12_STATIC_SAMPLER_DESC, 2> samplers{};
    samplers[0].Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    samplers[0].AddressU = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    samplers[0].AddressV = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    samplers[0].AddressW = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    samplers[0].MipLODBias = 0.0F;
    samplers[0].MaxAnisotropy = 1;
    samplers[0].ComparisonFunc = D3D12_COMPARISON_FUNC_ALWAYS;
    samplers[0].BorderColor = D3D12_STATIC_BORDER_COLOR_OPAQUE_WHITE;
    samplers[0].MinLOD = 0.0F;
    samplers[0].MaxLOD = D3D12_FLOAT32_MAX;
    samplers[0].ShaderRegister = 0;
    samplers[0].RegisterSpace = 0;
    samplers[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    samplers[1] = samplers[0];
    samplers[1].AddressU = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    samplers[1].AddressV = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    samplers[1].AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    samplers[1].ShaderRegister = 1;

    D3D12_ROOT_SIGNATURE_DESC root_desc{};
    root_desc.NumParameters = static_cast<UINT>(root_parameters.size());
    root_desc.pParameters = root_parameters.data();
    root_desc.NumStaticSamplers = static_cast<UINT>(samplers.size());
    root_desc.pStaticSamplers = samplers.data();
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
    set_debug_name(world_root_signature_.Get(), "OpenStrike World Root Signature");

    ComPtr<ID3DBlob> vertex_shader = load_shader_bytecode(engine_context_, world_material_vertex_shader_file());
    ComPtr<ID3DBlob> pixel_shader = load_shader_bytecode(engine_context_, world_material_pixel_shader_file());
    if (!vertex_shader || !pixel_shader)
    {
        return false;
    }

    std::array<D3D12_INPUT_ELEMENT_DESC, 5> input_layout{{
        {"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, offsetof(WorldDrawVertex, position), D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
        {"NORMAL", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, offsetof(WorldDrawVertex, normal), D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
        {"TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, offsetof(WorldDrawVertex, texcoord), D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
        {"TEXCOORD", 1, DXGI_FORMAT_R32G32_FLOAT, 0, offsetof(WorldDrawVertex, lightmap_texcoord), D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
        {"TEXCOORD", 2, DXGI_FORMAT_R32_FLOAT, 0, offsetof(WorldDrawVertex, lightmap_weight), D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
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

    if (!succeeded(device_->CreateGraphicsPipelineState(&pso_desc, IID_PPV_ARGS(&world_pipeline_state_)),
            "ID3D12Device::CreateGraphicsPipelineState world"))
    {
        return false;
    }
    set_debug_name(world_pipeline_state_.Get(), "OpenStrike World Pipeline State");
    return true;
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
    set_debug_name(skybox_root_signature_.Get(), "OpenStrike Skybox Root Signature");

    ComPtr<ID3DBlob> vertex_shader = load_shader_bytecode(engine_context_, skybox_vertex_shader_file());
    ComPtr<ID3DBlob> pixel_shader = load_shader_bytecode(engine_context_, skybox_pixel_shader_file());
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

    if (!succeeded(device_->CreateGraphicsPipelineState(&pso_desc, IID_PPV_ARGS(&skybox_pipeline_state_)),
            "ID3D12Device::CreateGraphicsPipelineState skybox"))
    {
        return false;
    }
    set_debug_name(skybox_pipeline_state_.Get(), "OpenStrike Skybox Pipeline State");
    return true;
}

bool Dx12Renderer::resize_swap_chain(std::uint32_t width, std::uint32_t height)
{
    if (!swap_chain_ || width == 0 || height == 0)
    {
        return true;
    }

    if (!wait_for_gpu())
    {
        return false;
    }
    render_targets_ = {};
    depth_buffer_.Reset();

    const UINT flags = allow_tearing_ ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING : 0U;
    if (!succeeded_with_device(device_.Get(),
            swap_chain_->ResizeBuffers(kFrameCount, width, height, DXGI_FORMAT_R8G8B8A8_UNORM, flags),
            "IDXGISwapChain::ResizeBuffers"))
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

    ShaderDescriptorRange rml_descriptor_range;
    if (!allocate_shader_descriptors(kRmlDescriptorCount, rml_descriptor_range))
    {
        return false;
    }

    rml_render_interface_ = std::make_unique<RmlDx12RenderInterface>();
    if (!rml_render_interface_->initialize(device_.Get(),
            command_queue_.Get(),
            shader_descriptor_heap_.Get(),
            shader_descriptor_size_,
            rml_descriptor_range.first_descriptor,
            rml_descriptor_range.descriptor_count))
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
    main_menu_controller_->set_launch_map_callback([this](std::string map_name, std::string game_mode_alias) {
        if (engine_context_ != nullptr)
        {
            if (!apply_game_mode_alias(engine_context_->variables, game_mode_alias, engine_context_->filesystem, map_name))
            {
                log_warning("menu selected unknown game mode alias '{}'", game_mode_alias);
            }
            engine_context_->loading_screen.open_for_map(
                map_name, current_game_mode_display_name(engine_context_->variables), default_loading_description(), default_loading_tip());
            engine_context_->loading_screen.set_progress(0.05F, "Retrieving game data...");
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
        rml_loading_screen_controller_ = std::make_unique<RmlLoadingScreenController>();
        if (!rml_loading_screen_controller_->initialize(*rml_context_, config))
        {
            return false;
        }

        rml_hud_controller_ = std::make_unique<RmlHudController>();
        if (!rml_hud_controller_->initialize(*rml_context_, *engine_context_, config))
        {
            return false;
        }

        rml_team_menu_controller_ = std::make_unique<RmlTeamMenuController>();
        if (!rml_team_menu_controller_->initialize(*rml_context_, *engine_context_, config))
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
    const bool loading_screen_active = engine_context_->loading_screen.visible();
    if (world_generation == main_menu_world_generation_ && loading_screen_active == main_menu_loading_active_)
    {
        return;
    }

    main_menu_world_generation_ = world_generation;
    main_menu_loading_active_ = loading_screen_active;
    if (loading_screen_active)
    {
        if (main_menu_controller_->visible())
        {
            main_menu_controller_->set_visible(false);
            log_info("hid RmlUi main menu for loading screen");
        }
    }
    else if (engine_context_->world.current_world() != nullptr)
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

bool Dx12Renderer::ensure_skybox_gpu_resources(const LoadedWorld& world)
{
    if (engine_context_ == nullptr || skybox_pipeline_state_ == nullptr || skybox_root_signature_ == nullptr || command_list_ == nullptr)
    {
        return false;
    }

    const std::uint64_t generation = engine_context_->world.generation();
    if (world.kind != WorldAssetKind::SourceBsp || !world.mesh.has_sky_surfaces)
    {
        if (skybox_gpu_generation_ != generation)
        {
            skybox_gpu_.reset();
            skybox_gpu_generation_ = generation;
        }
        return false;
    }

    const std::optional<std::string> requested_sky_name = skybox_name_from_world(world);
    if (!requested_sky_name || requested_sky_name->empty())
    {
        if (skybox_gpu_generation_ != generation)
        {
            skybox_gpu_.reset();
            skybox_gpu_generation_ = generation;
        }
        return false;
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
            return false;
        }

        auto gpu = std::make_unique<SkyboxGpuResources>();
        gpu->sky_name = *requested_sky_name;

        const std::array<SkyboxDrawVertex, kSkyboxFaceCount * 4> vertices = build_skybox_vertices();
        const std::array<std::uint16_t, kSkyboxFaceCount * 6> indices = build_skybox_indices();
        const auto vertex_bytes = static_cast<std::uint64_t>(vertices.size() * sizeof(SkyboxDrawVertex));
        const auto index_bytes = static_cast<std::uint64_t>(indices.size() * sizeof(std::uint16_t));
        if (!upload_static_buffer_to_gpu(device_.Get(),
                command_list_.Get(),
                vertices.data(),
                vertex_bytes,
                D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER,
                "OpenStrike Skybox Vertex Buffer",
                gpu->vertex_buffer,
                gpu->vertex_upload_buffer))
        {
            return false;
        }

        if (!upload_static_buffer_to_gpu(device_.Get(),
                command_list_.Get(),
                indices.data(),
                index_bytes,
                D3D12_RESOURCE_STATE_INDEX_BUFFER,
                "OpenStrike Skybox Index Buffer",
                gpu->index_buffer,
                gpu->index_upload_buffer))
        {
            return false;
        }

        gpu->vertex_view.BufferLocation = gpu->vertex_buffer->GetGPUVirtualAddress();
        gpu->vertex_view.SizeInBytes = static_cast<UINT>(vertex_bytes);
        gpu->vertex_view.StrideInBytes = sizeof(SkyboxDrawVertex);
        gpu->index_view.BufferLocation = gpu->index_buffer->GetGPUVirtualAddress();
        gpu->index_view.SizeInBytes = static_cast<UINT>(index_bytes);
        gpu->index_view.Format = DXGI_FORMAT_R16_UINT;

        if (!allocate_shader_descriptors(kSkyboxFaceCount, gpu->srv_range))
        {
            return false;
        }

        D3D12_CPU_DESCRIPTOR_HANDLE cpu_handle = gpu->srv_range.cpu_start;
        for (std::size_t face = 0; face < kSkyboxFaceCount; ++face)
        {
            const std::string texture_name = std::format("OpenStrike Skybox {} {}", resolved_sky_name, kSkyboxSuffixByDrawAxis[face]);
            if (!upload_source_texture_to_gpu(device_.Get(), command_list_.Get(), (*sky_textures)[face], cpu_handle, gpu->textures[face], texture_name))
            {
                log_warning("failed to upload skybox '{}' face '{}'", resolved_sky_name, kSkyboxSuffixByDrawAxis[face]);
                return false;
            }
            cpu_handle.ptr += shader_descriptor_size_;
        }

        log_info("uploaded skybox '{}' from Source faces", resolved_sky_name);
        skybox_gpu_ = std::move(gpu);
    }

    if (skybox_gpu_ == nullptr)
    {
        return false;
    }

    return true;
}

bool Dx12Renderer::record_skybox(ID3D12GraphicsCommandList* command_list, const LoadedWorld&) const
{
    if (command_list == nullptr || engine_context_ == nullptr || skybox_gpu_ == nullptr || skybox_pipeline_state_ == nullptr || skybox_root_signature_ == nullptr)
    {
        return false;
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
    D3D12_CPU_DESCRIPTOR_HANDLE rtv_handle = offset_descriptor(rtv_heap_->GetCPUDescriptorHandleForHeapStart(), frame_index_, rtv_descriptor_size_);
    D3D12_CPU_DESCRIPTOR_HANDLE dsv_handle = dsv_heap_->GetCPUDescriptorHandleForHeapStart();
    ID3D12DescriptorHeap* descriptor_heaps[] = {shader_descriptor_heap_.Get()};
    command_list->OMSetRenderTargets(1, &rtv_handle, FALSE, &dsv_handle);
    command_list->SetDescriptorHeaps(1, descriptor_heaps);
    command_list->SetPipelineState(skybox_pipeline_state_.Get());
    command_list->SetGraphicsRootSignature(skybox_root_signature_.Get());
    command_list->SetGraphicsRoot32BitConstants(0, static_cast<UINT>(constants.size()), constants.data(), 0);
    command_list->RSSetViewports(1, &viewport);
    command_list->RSSetScissorRects(1, &scissor);
    command_list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    command_list->IASetVertexBuffers(0, 1, &skybox_gpu_->vertex_view);
    command_list->IASetIndexBuffer(&skybox_gpu_->index_view);

    const D3D12_GPU_DESCRIPTOR_HANDLE gpu_start = skybox_gpu_->srv_range.gpu_start;
    for (std::uint32_t face = 0; face < kSkyboxFaceCount; ++face)
    {
        D3D12_GPU_DESCRIPTOR_HANDLE texture_handle = gpu_start;
        texture_handle.ptr += static_cast<UINT64>(face) * shader_descriptor_size_;
        command_list->SetGraphicsRootDescriptorTable(1, texture_handle);
        command_list->DrawIndexedInstanced(6, 1, face * 6, 0, 0);
    }
    return true;
}

bool Dx12Renderer::ensure_world_gpu_resources()
{
    if (engine_context_ == nullptr || world_pipeline_state_ == nullptr || world_root_signature_ == nullptr || command_list_ == nullptr)
    {
        return false;
    }

    const LoadedWorld* world = engine_context_->world.current_world();
    const std::uint64_t generation = engine_context_->world.generation();
    if (world == nullptr || world->mesh.vertices.empty() || world->mesh.indices.empty())
    {
        world_gpu_.reset();
        world_gpu_generation_ = generation;
        return false;
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
                {vertex.lightmap_texcoord.x, vertex.lightmap_texcoord.y},
                vertex.lightmap_weight,
            });
        }

        const auto vertex_bytes = static_cast<std::uint64_t>(vertices.size() * sizeof(WorldDrawVertex));
        const auto index_bytes = static_cast<std::uint64_t>(world->mesh.indices.size() * sizeof(std::uint32_t));
        auto gpu = std::make_unique<WorldGpuResources>();

        if (!upload_static_buffer_to_gpu(device_.Get(),
                command_list_.Get(),
                vertices.data(),
                vertex_bytes,
                D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER,
                std::format("OpenStrike World {} Vertex Buffer", world->name),
                gpu->vertex_buffer,
                gpu->vertex_upload_buffer))
        {
            return false;
        }

        if (!upload_static_buffer_to_gpu(device_.Get(),
                command_list_.Get(),
                world->mesh.indices.data(),
                index_bytes,
                D3D12_RESOURCE_STATE_INDEX_BUFFER,
                std::format("OpenStrike World {} Index Buffer", world->name),
                gpu->index_buffer,
                gpu->index_upload_buffer))
        {
            return false;
        }

        gpu->vertex_view.BufferLocation = gpu->vertex_buffer->GetGPUVirtualAddress();
        gpu->vertex_view.SizeInBytes = static_cast<UINT>(vertex_bytes);
        gpu->vertex_view.StrideInBytes = sizeof(WorldDrawVertex);
        gpu->index_view.BufferLocation = gpu->index_buffer->GetGPUVirtualAddress();
        gpu->index_view.SizeInBytes = static_cast<UINT>(index_bytes);
        gpu->index_view.Format = DXGI_FORMAT_R32_UINT;
        gpu->index_count = static_cast<std::uint32_t>(world->mesh.indices.size());

        const std::uint32_t material_count = std::max<std::uint32_t>(static_cast<std::uint32_t>(world->mesh.materials.size()), 1U);
        if (!allocate_shader_descriptors(material_count + kWorldLightmapDescriptorCount + (kFrameCount * kForwardPlusDescriptorCount), gpu->srv_range))
        {
            return false;
        }
        gpu->material_descriptor_count = material_count;
        gpu->textures.reserve(material_count);
        gpu->material_constants.reserve(material_count);

        SourceAssetStore source_assets(engine_context_->filesystem, &world->embedded_assets);
        MaterialSystem material_system(source_assets);
        D3D12_CPU_DESCRIPTOR_HANDLE cpu_handle = gpu->srv_range.cpu_start;
        for (std::uint32_t material_index = 0; material_index < material_count; ++material_index)
        {
            const WorldMaterial fallback_material{.name = "__missing", .width = 1, .height = 1};
            const WorldMaterial& material = material_index < world->mesh.materials.size() ? world->mesh.materials[material_index] : fallback_material;
            LoadedMaterial loaded_material = material_system.load_world_material(material.name);

            TextureGpuResources gpu_texture;
            if (!upload_source_texture_to_gpu(device_.Get(),
                    command_list_.Get(),
                    loaded_material.base_texture,
                    cpu_handle,
                    gpu_texture,
                    std::format("OpenStrike Material {}", material.name)))
            {
                SourceTexture fallback = material_system.fallback_texture(material.name);
                if (!upload_source_texture_to_gpu(device_.Get(),
                        command_list_.Get(),
                        fallback,
                        cpu_handle,
                        gpu_texture,
                        std::format("OpenStrike Fallback Material {}", material.name)))
                {
                    return false;
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
            cpu_handle.ptr += shader_descriptor_size_;
        }

        if (!upload_world_lightmap_to_gpu(device_.Get(),
                command_list_.Get(),
                world->mesh.lightmap_atlas,
                cpu_handle,
                gpu->lightmap_texture,
                std::format("OpenStrike World {} Lightmap Atlas", world->name)))
        {
            return false;
        }
        cpu_handle.ptr += shader_descriptor_size_;

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

    return world_gpu_ != nullptr;
}

bool Dx12Renderer::upload_forward_plus_resources(const LoadedWorld& world)
{
    if (device_ == nullptr || world_gpu_ == nullptr)
    {
        return false;
    }

    const std::uint32_t tile_count_x = std::max<std::uint32_t>(1U, (width_ + kForwardPlusTileSize - 1U) / kForwardPlusTileSize);
    const std::uint32_t tile_count_y = std::max<std::uint32_t>(1U, (height_ + kForwardPlusTileSize - 1U) / kForwardPlusTileSize);
    const std::uint32_t tile_count = tile_count_x * tile_count_y;
    const std::uint32_t real_light_count =
        std::min<std::uint32_t>(static_cast<std::uint32_t>(world.lights.size()), kForwardPlusMaxLights);

    std::vector<ForwardPlusLightGpu>& lights = world_gpu_->forward_plus_lights;
    lights.clear();
    lights.reserve(std::max<std::uint32_t>(real_light_count, 1U));
    for (std::uint32_t index = 0; index < real_light_count; ++index)
    {
        lights.push_back(forward_plus_light_gpu(world.lights[index]));
    }
    if (lights.empty())
    {
        lights.push_back({});
    }

    std::vector<ForwardPlusTileGpu>& tiles = world_gpu_->forward_plus_tiles;
    std::vector<std::uint32_t>& tile_counts = world_gpu_->forward_plus_tile_counts;
    std::vector<std::uint32_t>& tile_write_offsets = world_gpu_->forward_plus_tile_write_offsets;
    std::vector<std::uint32_t>& light_indices = world_gpu_->forward_plus_light_indices;
    std::vector<ForwardPlusTileBounds>& light_bounds = world_gpu_->forward_plus_light_bounds;
    std::vector<std::uint8_t>& light_visible = world_gpu_->forward_plus_light_visible;

    tiles.assign(tile_count, {});
    tile_counts.assign(tile_count, 0);
    tile_write_offsets.assign(tile_count, 0);
    light_bounds.resize(real_light_count);
    light_visible.assign(real_light_count, 0);

    if (real_light_count > 0)
    {
        const CameraState* camera = engine_context_ != nullptr ? &engine_context_->camera : nullptr;
        const std::array<float, 16> matrix = world_to_clip_matrix(world.mesh, camera, width_, height_);
        const bool perspective_camera = camera != nullptr && camera->active;
        for (std::uint32_t light_index = 0; light_index < real_light_count; ++light_index)
        {
            const std::optional<ForwardPlusTileBounds> bounds =
                light_tile_bounds(world.lights[light_index], matrix, perspective_camera, width_, height_, tile_count_x, tile_count_y);
            if (!bounds)
            {
                continue;
            }

            light_visible[light_index] = 1;
            light_bounds[light_index] = *bounds;
            for (std::uint32_t tile_y = bounds->min_y; tile_y <= bounds->max_y; ++tile_y)
            {
                for (std::uint32_t tile_x = bounds->min_x; tile_x <= bounds->max_x; ++tile_x)
                {
                    std::uint32_t& count = tile_counts[(tile_y * tile_count_x) + tile_x];
                    if (count < kForwardPlusMaxLightsPerTile)
                    {
                        ++count;
                    }
                }
            }
        }
    }

    std::uint32_t light_index_count = 0;
    for (std::uint32_t tile_index = 0; tile_index < tile_count; ++tile_index)
    {
        ForwardPlusTileGpu& tile = tiles[tile_index];
        tile.light_offset = light_index_count;
        tile.light_count = tile_counts[tile_index];
        tile_write_offsets[tile_index] = light_index_count;
        light_index_count += tile.light_count;
    }

    light_indices.assign(std::max<std::uint32_t>(light_index_count, 1U), 0);
    for (std::uint32_t light_index = 0; light_index < real_light_count; ++light_index)
    {
        if (light_visible[light_index] == 0)
        {
            continue;
        }

        const ForwardPlusTileBounds& bounds = light_bounds[light_index];
        for (std::uint32_t tile_y = bounds.min_y; tile_y <= bounds.max_y; ++tile_y)
        {
            for (std::uint32_t tile_x = bounds.min_x; tile_x <= bounds.max_x; ++tile_x)
            {
                const std::uint32_t tile_index = (tile_y * tile_count_x) + tile_x;
                const std::uint32_t tile_begin = tiles[tile_index].light_offset;
                const std::uint32_t tile_end = tile_begin + tiles[tile_index].light_count;
                std::uint32_t& write_offset = tile_write_offsets[tile_index];
                if (write_offset < tile_end)
                {
                    light_indices[write_offset++] = light_index;
                }
            }
        }
    }

    ForwardPlusFrameGpuResources& frame = world_gpu_->forward_plus_frames[frame_index_];
    const std::uint32_t required_tile_capacity = std::max<std::uint32_t>(static_cast<std::uint32_t>(tiles.size()), 1U);
    const std::uint32_t required_index_capacity = std::max<std::uint32_t>(static_cast<std::uint32_t>(light_indices.size()), 1U);
    const std::uint32_t required_light_capacity = std::max<std::uint32_t>(static_cast<std::uint32_t>(lights.size()), 1U);
    bool descriptors_dirty = false;

    if (frame.tile_capacity < required_tile_capacity)
    {
        if (!create_upload_buffer(device_.Get(),
                static_cast<std::uint64_t>(required_tile_capacity) * sizeof(ForwardPlusTileGpu),
                std::format("OpenStrike Forward+ Tile Buffer Frame {}", frame_index_),
                frame.tile_buffer))
        {
            return false;
        }
        frame.tile_capacity = required_tile_capacity;
        descriptors_dirty = true;
    }

    if (frame.light_index_capacity < required_index_capacity)
    {
        if (!create_upload_buffer(device_.Get(),
                static_cast<std::uint64_t>(required_index_capacity) * sizeof(std::uint32_t),
                std::format("OpenStrike Forward+ Light Index Buffer Frame {}", frame_index_),
                frame.light_index_buffer))
        {
            return false;
        }
        frame.light_index_capacity = required_index_capacity;
        descriptors_dirty = true;
    }

    if (frame.light_capacity < required_light_capacity)
    {
        if (!create_upload_buffer(device_.Get(),
                static_cast<std::uint64_t>(required_light_capacity) * sizeof(ForwardPlusLightGpu),
                std::format("OpenStrike Forward+ Light Buffer Frame {}", frame_index_),
                frame.light_buffer))
        {
            return false;
        }
        frame.light_capacity = required_light_capacity;
        descriptors_dirty = true;
    }

    if (!upload_buffer_data(frame.tile_buffer.Get(),
            tiles.data(),
            static_cast<std::uint64_t>(tiles.size()) * sizeof(ForwardPlusTileGpu),
            "ID3D12Resource::Map forward+ tiles") ||
        !upload_buffer_data(frame.light_index_buffer.Get(),
            light_indices.data(),
            static_cast<std::uint64_t>(light_indices.size()) * sizeof(std::uint32_t),
            "ID3D12Resource::Map forward+ light indices") ||
        !upload_buffer_data(frame.light_buffer.Get(),
            lights.data(),
            static_cast<std::uint64_t>(lights.size()) * sizeof(ForwardPlusLightGpu),
            "ID3D12Resource::Map forward+ lights"))
    {
        return false;
    }

    if (descriptors_dirty)
    {
        D3D12_CPU_DESCRIPTOR_HANDLE cpu_handle = world_gpu_->srv_range.cpu_start;
        cpu_handle.ptr += static_cast<UINT64>(
                              world_gpu_->material_descriptor_count + kWorldLightmapDescriptorCount + (frame_index_ * kForwardPlusDescriptorCount)) *
                          shader_descriptor_size_;
        create_structured_buffer_srv(device_.Get(), frame.tile_buffer.Get(), frame.tile_capacity, sizeof(ForwardPlusTileGpu), cpu_handle);
        cpu_handle.ptr += shader_descriptor_size_;
        create_structured_buffer_srv(device_.Get(), frame.light_index_buffer.Get(), frame.light_index_capacity, sizeof(std::uint32_t), cpu_handle);
        cpu_handle.ptr += shader_descriptor_size_;
        create_structured_buffer_srv(device_.Get(), frame.light_buffer.Get(), frame.light_capacity, sizeof(ForwardPlusLightGpu), cpu_handle);
    }

    world_gpu_->forward_plus_light_count = real_light_count;
    return true;
}

bool Dx12Renderer::record_world(ID3D12GraphicsCommandList* command_list) const
{
    if (command_list == nullptr || engine_context_ == nullptr || world_gpu_ == nullptr || world_pipeline_state_ == nullptr || world_root_signature_ == nullptr)
    {
        return false;
    }

    const LoadedWorld* world = engine_context_->world.current_world();
    if (world == nullptr || world->mesh.vertices.empty() || world->mesh.indices.empty())
    {
        return false;
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

    std::array<float, kWorldShaderFloatCount> constants = world_shader_constants(world->mesh, &engine_context_->camera, width_, height_);
    constants[kWorldTransformFloatCount + 11] = static_cast<float>(world_gpu_->forward_plus_light_count);
    D3D12_CPU_DESCRIPTOR_HANDLE rtv_handle = offset_descriptor(rtv_heap_->GetCPUDescriptorHandleForHeapStart(), frame_index_, rtv_descriptor_size_);
    D3D12_CPU_DESCRIPTOR_HANDLE dsv_handle = dsv_heap_->GetCPUDescriptorHandleForHeapStart();
    ID3D12DescriptorHeap* descriptor_heaps[] = {shader_descriptor_heap_.Get()};
    command_list->OMSetRenderTargets(1, &rtv_handle, FALSE, &dsv_handle);
    command_list->SetDescriptorHeaps(1, descriptor_heaps);
    command_list->SetPipelineState(world_pipeline_state_.Get());
    command_list->SetGraphicsRootSignature(world_root_signature_.Get());
    command_list->SetGraphicsRoot32BitConstants(0, static_cast<UINT>(constants.size()), constants.data(), 0);
    command_list->RSSetViewports(1, &viewport);
    command_list->RSSetScissorRects(1, &scissor);
    command_list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    command_list->IASetVertexBuffers(0, 1, &world_gpu_->vertex_view);
    command_list->IASetIndexBuffer(&world_gpu_->index_view);
    const D3D12_GPU_DESCRIPTOR_HANDLE gpu_start = world_gpu_->srv_range.gpu_start;
    D3D12_GPU_DESCRIPTOR_HANDLE lightmap_handle = gpu_start;
    lightmap_handle.ptr += static_cast<UINT64>(world_gpu_->material_descriptor_count) * shader_descriptor_size_;
    command_list->SetGraphicsRootDescriptorTable(3, lightmap_handle);
    D3D12_GPU_DESCRIPTOR_HANDLE forward_plus_handle = gpu_start;
    forward_plus_handle.ptr += static_cast<UINT64>(
                                   world_gpu_->material_descriptor_count + kWorldLightmapDescriptorCount + (frame_index_ * kForwardPlusDescriptorCount)) *
                               shader_descriptor_size_;
    command_list->SetGraphicsRootDescriptorTable(4, forward_plus_handle);
    std::uint32_t last_material_index = std::numeric_limits<std::uint32_t>::max();
    for (const WorldDrawBatch& batch : world_gpu_->batches)
    {
        if (batch.index_count == 0)
        {
            continue;
        }

        if (batch.material_index != last_material_index)
        {
            D3D12_GPU_DESCRIPTOR_HANDLE texture_handle = gpu_start;
            texture_handle.ptr += static_cast<UINT64>(batch.material_index) * shader_descriptor_size_;
            command_list->SetGraphicsRootDescriptorTable(1, texture_handle);
            command_list->SetGraphicsRoot32BitConstants(2,
                static_cast<UINT>(sizeof(MaterialGpuConstants) / sizeof(std::uint32_t)),
                &world_gpu_->material_constants[batch.material_index],
                0);
            last_material_index = batch.material_index;
        }
        command_list->DrawIndexedInstanced(batch.index_count, 1, batch.first_index, 0, 0);
    }
    return true;
}

void Dx12Renderer::shutdown_rml()
{
    rml_console_controller_.reset();
    rml_team_menu_controller_.reset();
    rml_hud_controller_.reset();
    rml_loading_screen_controller_.reset();
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

        const bool ui_visible = gameplay_ui_visible(rml_console_controller_.get(),
            main_menu_controller_.get(),
            rml_loading_screen_controller_.get(),
            rml_team_menu_controller_.get());
        if (engine_context_ != nullptr && !ui_visible)
        {
            if (handle_sdl_gameplay_input_event(engine_context_->input, event))
            {
                continue;
            }
        }

        if (rml_context_ != nullptr)
        {
            RmlSDL::InputEventHandler(rml_context_, window_, event);
        }
    }

    if (engine_context_ != nullptr)
    {
        const bool should_capture = engine_context_->world.current_world() != nullptr &&
                                    !gameplay_ui_visible(rml_console_controller_.get(),
                                        main_menu_controller_.get(),
                                        rml_loading_screen_controller_.get(),
                                        rml_team_menu_controller_.get());
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

bool Dx12Renderer::wait_for_gpu()
{
    if (!command_queue_ || !fence_ || fence_event_ == nullptr)
    {
        return true;
    }

    const std::uint64_t signal_value = fence_value_++;
    if (!succeeded_with_device(device_.Get(), command_queue_->Signal(fence_.Get(), signal_value), "ID3D12CommandQueue::Signal wait_for_gpu"))
    {
        return false;
    }

    if (fence_->GetCompletedValue() < signal_value)
    {
        if (!succeeded_with_device(device_.Get(),
                fence_->SetEventOnCompletion(signal_value, fence_event_),
                "ID3D12Fence::SetEventOnCompletion wait_for_gpu") ||
            !wait_for_fence_event(fence_event_, "Dx12Renderer::wait_for_gpu"))
        {
            return false;
        }
    }

    for (std::uint64_t& value : frame_fence_values_)
    {
        value = signal_value;
    }
    return true;
}

bool Dx12Renderer::move_to_next_frame()
{
    const std::uint64_t current_fence_value = fence_value_;
    if (!succeeded_with_device(device_.Get(), command_queue_->Signal(fence_.Get(), current_fence_value), "ID3D12CommandQueue::Signal frame"))
    {
        return false;
    }
    frame_fence_values_[frame_index_] = current_fence_value;
    ++fence_value_;

    frame_index_ = swap_chain_->GetCurrentBackBufferIndex();

    if (fence_->GetCompletedValue() < frame_fence_values_[frame_index_])
    {
        if (!succeeded_with_device(device_.Get(),
                fence_->SetEventOnCompletion(frame_fence_values_[frame_index_], fence_event_),
                "ID3D12Fence::SetEventOnCompletion frame") ||
            !wait_for_fence_event(fence_event_, "Dx12Renderer::move_to_next_frame"))
        {
            return false;
        }
    }
    return true;
}
}
