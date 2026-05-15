#include "openstrike/renderer/rml_dx12_render_interface.hpp"

#if defined(_WIN32)

#include "openstrike/core/log.hpp"

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
#include <wincodec.h>
#include <wrl/client.h>

#pragma comment(lib, "windowscodecs.lib")

#include <RmlUi/Core/Core.h>
#include <RmlUi/Core/FileInterface.h>
#include <RmlUi/Core/Vertex.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <format>
#include <limits>
#include <memory>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

using Microsoft::WRL::ComPtr;

namespace openstrike
{
namespace
{
constexpr std::uint32_t kFrameCount = 2;
constexpr std::uint32_t kMaxTextureDescriptors = 128;
constexpr DXGI_FORMAT kRenderTargetFormat = DXGI_FORMAT_R8G8B8A8_UNORM;
constexpr DXGI_FORMAT kTextureFormat = DXGI_FORMAT_R8G8B8A8_UNORM;

const char* kVertexShader = R"(
cbuffer ViewportConstants : register(b0)
{
    float2 viewport_size;
};

struct VSInput
{
    float2 position : POSITION;
    float4 color : COLOR0;
    float2 texcoord : TEXCOORD0;
};

struct PSInput
{
    float4 position : SV_POSITION;
    float4 color : COLOR0;
    float2 texcoord : TEXCOORD0;
};

PSInput main(VSInput input)
{
    PSInput output;
    const float2 ndc = float2((input.position.x / viewport_size.x) * 2.0f - 1.0f,
                              1.0f - (input.position.y / viewport_size.y) * 2.0f);
    output.position = float4(ndc, 0.0f, 1.0f);
    output.color = input.color;
    output.texcoord = input.texcoord;
    return output;
}
)";

const char* kPixelShader = R"(
Texture2D rml_texture : register(t0);
SamplerState rml_sampler : register(s0);

struct PSInput
{
    float4 position : SV_POSITION;
    float4 color : COLOR0;
    float2 texcoord : TEXCOORD0;
};

float4 main(PSInput input) : SV_TARGET
{
    return rml_texture.Sample(rml_sampler, input.texcoord) * input.color;
}
)";

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
    desc.Width = size;
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

D3D12_RESOURCE_DESC texture_desc(std::uint32_t width, std::uint32_t height)
{
    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    desc.Alignment = 0;
    desc.Width = width;
    desc.Height = height;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.Format = kTextureFormat;
    desc.SampleDesc.Count = 1;
    desc.SampleDesc.Quality = 0;
    desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    desc.Flags = D3D12_RESOURCE_FLAG_NONE;
    return desc;
}

std::string lower_copy(std::string_view text)
{
    std::string result(text);
    std::transform(result.begin(), result.end(), result.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    return result;
}

std::string normalize_texture_source(std::string source)
{
    std::replace(source.begin(), source.end(), '\\', '/');
    if (source.rfind("file:///", 0) == 0)
    {
        source.erase(0, 8);
    }
    else if (source.rfind("file://", 0) == 0)
    {
        source.erase(0, 7);
    }

    if (source.size() >= 3 && source[0] == '/' && source[2] == ':')
    {
        source.erase(0, 1);
    }

    return source;
}

bool is_video_texture_source(std::string_view normalized_source)
{
    const std::string lower = lower_copy(normalized_source);
    return lower.find("/__video/") != std::string::npos || lower.ends_with(".webm");
}

std::wstring utf8_to_wide(std::string_view text)
{
    if (text.empty())
    {
        return {};
    }

    int size = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()), nullptr, 0);
    if (size == 0)
    {
        size = MultiByteToWideChar(CP_ACP, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
    }
    if (size == 0)
    {
        return {};
    }

    std::wstring result(static_cast<std::size_t>(size), L'\0');
    const UINT code_page = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()), result.data(), size) != 0
        ? CP_UTF8
        : CP_ACP;
    if (code_page == CP_ACP)
    {
        MultiByteToWideChar(CP_ACP, 0, text.data(), static_cast<int>(text.size()), result.data(), size);
    }
    return result;
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
    if (byte_count > static_cast<std::uint64_t>(std::numeric_limits<std::size_t>::max()) ||
        byte_count > static_cast<std::uint64_t>(std::numeric_limits<UINT>::max()))
    {
        return false;
    }

    out_byte_size = static_cast<std::size_t>(byte_count);
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
            log_error("shader compile error: {}", static_cast<const char*>(errors->GetBufferPointer()));
        }
        else
        {
            log_error("{}", hresult_message("D3DCompile", result));
        }
        return {};
    }

    return shader;
}

struct Geometry
{
    std::vector<Rml::Vertex> vertices;
    std::vector<int> indices;
};

struct Texture
{
    ComPtr<ID3D12Resource> resource;
    D3D12_CPU_DESCRIPTOR_HANDLE cpu_handle{};
    D3D12_GPU_DESCRIPTOR_HANDLE gpu_handle{};
    Rml::Vector2i dimensions{};
};
}

struct RmlDx12RenderInterface::Impl
{
    [[nodiscard]] bool initialize(ID3D12Device* in_device, ID3D12CommandQueue* in_command_queue)
    {
        device = in_device;
        command_queue = in_command_queue;
        if (device == nullptr || command_queue == nullptr)
        {
            log_error("RmlUi DX12 renderer requires a valid device and command queue");
            return false;
        }

        if (!create_wic_factory() || !create_descriptor_heap() || !create_upload_objects() || !create_pipeline() || !create_white_texture())
        {
            shutdown();
            return false;
        }

        initialized = true;
        return true;
    }

    void shutdown()
    {
        wait_for_uploads();

        for (auto& uploads : frame_uploads)
        {
            uploads.clear();
        }

        textures.clear();
        white_texture = nullptr;
        pipeline_state.Reset();
        root_signature.Reset();
        srv_heap.Reset();
        upload_command_list.Reset();
        upload_allocator.Reset();
        upload_fence.Reset();
        wic_factory.Reset();

        if (upload_fence_event != nullptr)
        {
            CloseHandle(upload_fence_event);
            upload_fence_event = nullptr;
        }

        command_list = nullptr;
        command_queue = nullptr;
        device = nullptr;
        initialized = false;
        next_texture_descriptor = 0;

        if (com_initialized)
        {
            CoUninitialize();
            com_initialized = false;
        }
    }

    void begin_frame(ID3D12GraphicsCommandList* in_command_list, std::uint32_t in_frame_index, std::uint32_t in_width, std::uint32_t in_height)
    {
        command_list = in_command_list;
        frame_index = in_frame_index % kFrameCount;
        width = in_width;
        height = in_height;
        frame_uploads[frame_index].clear();
    }

    void end_frame()
    {
        command_list = nullptr;
    }

    [[nodiscard]] bool create_wic_factory()
    {
        const HRESULT co_result = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        if (SUCCEEDED(co_result))
        {
            com_initialized = true;
        }
        else if (co_result != RPC_E_CHANGED_MODE)
        {
            log_error("{}", hresult_message("CoInitializeEx for RmlUi WIC textures", co_result));
            return false;
        }

        const HRESULT factory_result = CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&wic_factory));
        if (FAILED(factory_result))
        {
            log_error("{}", hresult_message("CoCreateInstance CLSID_WICImagingFactory", factory_result));
            if (com_initialized)
            {
                CoUninitialize();
                com_initialized = false;
            }
            return false;
        }

        return true;
    }

    [[nodiscard]] bool create_descriptor_heap()
    {
        D3D12_DESCRIPTOR_HEAP_DESC desc{};
        desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
        desc.NumDescriptors = kMaxTextureDescriptors;
        desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        if (!succeeded(device->CreateDescriptorHeap(&desc, IID_PPV_ARGS(&srv_heap)), "ID3D12Device::CreateDescriptorHeap RmlUi SRV"))
        {
            return false;
        }

        descriptor_size = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        return true;
    }

    [[nodiscard]] bool create_upload_objects()
    {
        if (!succeeded(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&upload_allocator)),
                "ID3D12Device::CreateCommandAllocator RmlUi upload"))
        {
            return false;
        }

        if (!succeeded(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, upload_allocator.Get(), nullptr,
                IID_PPV_ARGS(&upload_command_list)), "ID3D12Device::CreateCommandList RmlUi upload"))
        {
            return false;
        }
        upload_command_list->Close();

        if (!succeeded(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&upload_fence)), "ID3D12Device::CreateFence RmlUi upload"))
        {
            return false;
        }

        upload_fence_event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (upload_fence_event == nullptr)
        {
            log_error("CreateEventW failed for RmlUi upload fence");
            return false;
        }

        upload_fence_value = 1;
        return true;
    }

    [[nodiscard]] bool create_pipeline()
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
        root_parameters[0].Constants.Num32BitValues = 2;

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
        sampler.BorderColor = D3D12_STATIC_BORDER_COLOR_TRANSPARENT_BLACK;
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
                log_error("root signature error: {}", static_cast<const char*>(signature_errors->GetBufferPointer()));
            }
            else
            {
                log_error("{}", hresult_message("D3D12SerializeRootSignature", serialize_result));
            }
            return false;
        }

        if (!succeeded(device->CreateRootSignature(0, signature_blob->GetBufferPointer(), signature_blob->GetBufferSize(),
                IID_PPV_ARGS(&root_signature)), "ID3D12Device::CreateRootSignature RmlUi"))
        {
            return false;
        }

        ComPtr<ID3DBlob> vertex_shader = compile_shader(kVertexShader, "main", "vs_5_0");
        ComPtr<ID3DBlob> pixel_shader = compile_shader(kPixelShader, "main", "ps_5_0");
        if (!vertex_shader || !pixel_shader)
        {
            return false;
        }

        std::array<D3D12_INPUT_ELEMENT_DESC, 3> input_layout{{
            {"POSITION", 0, DXGI_FORMAT_R32G32_FLOAT, 0, offsetof(Rml::Vertex, position), D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
            {"COLOR", 0, DXGI_FORMAT_R8G8B8A8_UNORM, 0, offsetof(Rml::Vertex, colour), D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
            {"TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, offsetof(Rml::Vertex, tex_coord), D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
        }};

        D3D12_GRAPHICS_PIPELINE_STATE_DESC pso_desc{};
        pso_desc.InputLayout = {input_layout.data(), static_cast<UINT>(input_layout.size())};
        pso_desc.pRootSignature = root_signature.Get();
        pso_desc.VS = {vertex_shader->GetBufferPointer(), vertex_shader->GetBufferSize()};
        pso_desc.PS = {pixel_shader->GetBufferPointer(), pixel_shader->GetBufferSize()};
        pso_desc.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
        pso_desc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
        pso_desc.RasterizerState.FrontCounterClockwise = FALSE;
        pso_desc.RasterizerState.DepthBias = D3D12_DEFAULT_DEPTH_BIAS;
        pso_desc.RasterizerState.DepthBiasClamp = D3D12_DEFAULT_DEPTH_BIAS_CLAMP;
        pso_desc.RasterizerState.SlopeScaledDepthBias = D3D12_DEFAULT_SLOPE_SCALED_DEPTH_BIAS;
        pso_desc.RasterizerState.DepthClipEnable = TRUE;
        pso_desc.BlendState.RenderTarget[0].BlendEnable = TRUE;
        pso_desc.BlendState.RenderTarget[0].SrcBlend = D3D12_BLEND_ONE;
        pso_desc.BlendState.RenderTarget[0].DestBlend = D3D12_BLEND_INV_SRC_ALPHA;
        pso_desc.BlendState.RenderTarget[0].BlendOp = D3D12_BLEND_OP_ADD;
        pso_desc.BlendState.RenderTarget[0].SrcBlendAlpha = D3D12_BLEND_ONE;
        pso_desc.BlendState.RenderTarget[0].DestBlendAlpha = D3D12_BLEND_INV_SRC_ALPHA;
        pso_desc.BlendState.RenderTarget[0].BlendOpAlpha = D3D12_BLEND_OP_ADD;
        pso_desc.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
        pso_desc.DepthStencilState.DepthEnable = FALSE;
        pso_desc.DepthStencilState.StencilEnable = FALSE;
        pso_desc.SampleMask = std::numeric_limits<UINT>::max();
        pso_desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        pso_desc.NumRenderTargets = 1;
        pso_desc.RTVFormats[0] = kRenderTargetFormat;
        pso_desc.SampleDesc.Count = 1;

        return succeeded(device->CreateGraphicsPipelineState(&pso_desc, IID_PPV_ARGS(&pipeline_state)),
            "ID3D12Device::CreateGraphicsPipelineState RmlUi");
    }

    [[nodiscard]] bool create_white_texture()
    {
        const std::array<Rml::byte, 4> pixel{255, 255, 255, 255};
        white_texture = create_texture(pixel.data(), 1, 1, "white");
        return white_texture != nullptr;
    }

    Texture* create_video_placeholder(const std::string& source, Rml::Vector2i& texture_dimensions)
    {
        constexpr std::uint32_t placeholder_width = 180;
        constexpr std::uint32_t placeholder_height = 52;
        std::vector<Rml::byte> pixels = make_go_button_placeholder(placeholder_width, placeholder_height);
        Texture* texture = create_texture(pixels.data(), placeholder_width, placeholder_height, "RmlUi Source video placeholder");
        if (texture == nullptr)
        {
            return nullptr;
        }

        texture_dimensions = texture->dimensions;
        log_info("using static RmlUi texture for Source video token '{}'", source);
        return texture;
    }

    Texture* load_texture_file(const std::string& source, Rml::Vector2i& texture_dimensions)
    {
        if (!wic_factory)
        {
            log_warning("RmlUi WIC texture factory is not initialized");
            return nullptr;
        }

        const std::wstring wide_path = utf8_to_wide(source);
        if (wide_path.empty())
        {
            log_warning("Could not convert RmlUi texture path '{}'", source);
            return nullptr;
        }

        std::error_code exists_error;
        if (!std::filesystem::is_regular_file(std::filesystem::path(wide_path), exists_error))
        {
            log_warning("RmlUi texture file does not exist: '{}'", source);
            return nullptr;
        }

        ComPtr<IWICBitmapDecoder> decoder;
        HRESULT result = wic_factory->CreateDecoderFromFilename(wide_path.c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnLoad,
            &decoder);
        if (FAILED(result))
        {
            log_warning("{} for '{}'", hresult_message("IWICImagingFactory::CreateDecoderFromFilename", result), source);
            return nullptr;
        }

        ComPtr<IWICBitmapFrameDecode> frame;
        result = decoder->GetFrame(0, &frame);
        if (FAILED(result))
        {
            log_warning("{} for '{}'", hresult_message("IWICBitmapDecoder::GetFrame", result), source);
            return nullptr;
        }

        UINT texture_width = 0;
        UINT texture_height = 0;
        result = frame->GetSize(&texture_width, &texture_height);
        if (FAILED(result))
        {
            log_warning("{} for '{}'", hresult_message("IWICBitmapFrameDecode::GetSize", result), source);
            return nullptr;
        }

        std::size_t byte_size = 0;
        if (!checked_texture_byte_size(texture_width, texture_height, byte_size))
        {
            log_warning("RmlUi texture '{}' has invalid dimensions {}x{}", source, texture_width, texture_height);
            return nullptr;
        }

        ComPtr<IWICFormatConverter> converter;
        result = wic_factory->CreateFormatConverter(&converter);
        if (FAILED(result))
        {
            log_warning("{} for '{}'", hresult_message("IWICImagingFactory::CreateFormatConverter", result), source);
            return nullptr;
        }

        result = converter->Initialize(frame.Get(), GUID_WICPixelFormat32bppRGBA, WICBitmapDitherTypeNone, nullptr, 0.0,
            WICBitmapPaletteTypeCustom);
        if (FAILED(result))
        {
            log_warning("{} for '{}'", hresult_message("IWICFormatConverter::Initialize", result), source);
            return nullptr;
        }

        std::vector<Rml::byte> pixels(byte_size);
        const UINT stride = texture_width * 4U;
        result = converter->CopyPixels(nullptr, stride, static_cast<UINT>(pixels.size()), pixels.data());
        if (FAILED(result))
        {
            log_warning("{} for '{}'", hresult_message("IWICBitmapSource::CopyPixels", result), source);
            return nullptr;
        }

        premultiply_alpha(pixels);
        Texture* texture = create_texture(pixels.data(), texture_width, texture_height, source.c_str());
        if (texture == nullptr)
        {
            return nullptr;
        }

        texture_dimensions = texture->dimensions;
        return texture;
    }

    Texture* create_texture(const Rml::byte* pixels, std::uint32_t texture_width, std::uint32_t texture_height, const char* debug_name)
    {
        if (next_texture_descriptor >= kMaxTextureDescriptors)
        {
            log_error("RmlUi texture descriptor heap is full");
            return nullptr;
        }

        auto texture = std::make_unique<Texture>();
        texture->dimensions = Rml::Vector2i(static_cast<int>(texture_width), static_cast<int>(texture_height));
        texture->cpu_handle = srv_heap->GetCPUDescriptorHandleForHeapStart();
        texture->cpu_handle.ptr += static_cast<SIZE_T>(next_texture_descriptor) * descriptor_size;
        texture->gpu_handle = srv_heap->GetGPUDescriptorHandleForHeapStart();
        texture->gpu_handle.ptr += static_cast<UINT64>(next_texture_descriptor) * descriptor_size;
        ++next_texture_descriptor;

        const auto default_heap = heap_properties(D3D12_HEAP_TYPE_DEFAULT);
        const auto desc = texture_desc(texture_width, texture_height);
        if (!succeeded(device->CreateCommittedResource(&default_heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                IID_PPV_ARGS(&texture->resource)), "ID3D12Device::CreateCommittedResource RmlUi texture"))
        {
            return nullptr;
        }

        if (debug_name != nullptr)
        {
            const std::wstring wide_name(debug_name, debug_name + std::strlen(debug_name));
            texture->resource->SetName(wide_name.c_str());
        }

        D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
        UINT num_rows = 0;
        UINT64 row_size_in_bytes = 0;
        UINT64 upload_size = 0;
        device->GetCopyableFootprints(&desc, 0, 1, 0, &footprint, &num_rows, &row_size_in_bytes, &upload_size);

        ComPtr<ID3D12Resource> upload_buffer;
        const auto upload_heap = heap_properties(D3D12_HEAP_TYPE_UPLOAD);
        const auto upload_desc = buffer_desc(upload_size);
        if (!succeeded(device->CreateCommittedResource(&upload_heap, D3D12_HEAP_FLAG_NONE, &upload_desc, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                IID_PPV_ARGS(&upload_buffer)), "ID3D12Device::CreateCommittedResource RmlUi texture upload"))
        {
            return nullptr;
        }

        Rml::byte* mapped = nullptr;
        if (!succeeded(upload_buffer->Map(0, nullptr, reinterpret_cast<void**>(&mapped)), "ID3D12Resource::Map RmlUi texture upload"))
        {
            return nullptr;
        }

        const std::uint64_t source_row_pitch = static_cast<std::uint64_t>(texture_width) * 4;
        Rml::byte* destination = mapped + footprint.Offset;
        for (std::uint32_t y = 0; y < texture_height; ++y)
        {
            std::memcpy(destination + static_cast<std::uint64_t>(y) * footprint.Footprint.RowPitch, pixels + static_cast<std::uint64_t>(y) * source_row_pitch,
                source_row_pitch);
        }
        upload_buffer->Unmap(0, nullptr);

        wait_for_uploads();
        upload_allocator->Reset();
        upload_command_list->Reset(upload_allocator.Get(), nullptr);

        D3D12_TEXTURE_COPY_LOCATION dst{};
        dst.pResource = texture->resource.Get();
        dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        dst.SubresourceIndex = 0;

        D3D12_TEXTURE_COPY_LOCATION src{};
        src.pResource = upload_buffer.Get();
        src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        src.PlacedFootprint = footprint;

        upload_command_list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);

        D3D12_RESOURCE_BARRIER barrier{};
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Transition.pResource = texture->resource.Get();
        barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
        barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
        barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        upload_command_list->ResourceBarrier(1, &barrier);

        upload_command_list->Close();
        ID3D12CommandList* lists[] = {upload_command_list.Get()};
        command_queue->ExecuteCommandLists(1, lists);
        signal_upload_fence();
        wait_for_uploads();

        D3D12_SHADER_RESOURCE_VIEW_DESC srv_desc{};
        srv_desc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srv_desc.Format = kTextureFormat;
        srv_desc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        srv_desc.Texture2D.MipLevels = 1;
        device->CreateShaderResourceView(texture->resource.Get(), &srv_desc, texture->cpu_handle);

        Texture* result = texture.get();
        textures.push_back(std::move(texture));
        return result;
    }

    void wait_for_uploads()
    {
        if (!upload_fence || upload_fence_event == nullptr)
        {
            return;
        }

        if (upload_fence->GetCompletedValue() < last_upload_signal)
        {
            upload_fence->SetEventOnCompletion(last_upload_signal, upload_fence_event);
            WaitForSingleObject(upload_fence_event, INFINITE);
        }
    }

    void signal_upload_fence()
    {
        last_upload_signal = upload_fence_value++;
        command_queue->Signal(upload_fence.Get(), last_upload_signal);
    }

    void render_geometry(const Geometry& geometry, Rml::Vector2f translation, Texture* texture)
    {
        if (!initialized || command_list == nullptr || geometry.vertices.empty() || geometry.indices.empty())
        {
            return;
        }

        std::vector<Rml::Vertex> translated_vertices = geometry.vertices;
        for (Rml::Vertex& vertex : translated_vertices)
        {
            vertex.position.x += translation.x;
            vertex.position.y += translation.y;
        }

        const auto vertex_bytes = static_cast<std::uint64_t>(translated_vertices.size() * sizeof(Rml::Vertex));
        const auto index_bytes = static_cast<std::uint64_t>(geometry.indices.size() * sizeof(int));

        ComPtr<ID3D12Resource> vertex_buffer;
        ComPtr<ID3D12Resource> index_buffer;
        const auto upload_heap = heap_properties(D3D12_HEAP_TYPE_UPLOAD);
        const auto vertex_desc = buffer_desc(vertex_bytes);
        const auto index_desc = buffer_desc(index_bytes);

        if (!succeeded(device->CreateCommittedResource(&upload_heap, D3D12_HEAP_FLAG_NONE, &vertex_desc, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                IID_PPV_ARGS(&vertex_buffer)), "ID3D12Device::CreateCommittedResource RmlUi vertex buffer"))
        {
            return;
        }

        if (!succeeded(device->CreateCommittedResource(&upload_heap, D3D12_HEAP_FLAG_NONE, &index_desc, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                IID_PPV_ARGS(&index_buffer)), "ID3D12Device::CreateCommittedResource RmlUi index buffer"))
        {
            return;
        }

        void* mapped = nullptr;
        if (!succeeded(vertex_buffer->Map(0, nullptr, &mapped), "ID3D12Resource::Map RmlUi vertex buffer"))
        {
            return;
        }
        std::memcpy(mapped, translated_vertices.data(), static_cast<std::size_t>(vertex_bytes));
        vertex_buffer->Unmap(0, nullptr);

        if (!succeeded(index_buffer->Map(0, nullptr, &mapped), "ID3D12Resource::Map RmlUi index buffer"))
        {
            return;
        }
        std::memcpy(mapped, geometry.indices.data(), static_cast<std::size_t>(index_bytes));
        index_buffer->Unmap(0, nullptr);

        D3D12_VIEWPORT viewport{};
        viewport.TopLeftX = 0.0F;
        viewport.TopLeftY = 0.0F;
        viewport.Width = static_cast<float>(width);
        viewport.Height = static_cast<float>(height);
        viewport.MinDepth = 0.0F;
        viewport.MaxDepth = 1.0F;

        D3D12_RECT scissor{};
        if (scissor_enabled)
        {
            scissor.left = std::clamp(scissor_region.Left(), 0, static_cast<int>(width));
            scissor.top = std::clamp(scissor_region.Top(), 0, static_cast<int>(height));
            scissor.right = std::clamp(scissor_region.Right(), 0, static_cast<int>(width));
            scissor.bottom = std::clamp(scissor_region.Bottom(), 0, static_cast<int>(height));
        }
        else
        {
            scissor.left = 0;
            scissor.top = 0;
            scissor.right = static_cast<LONG>(width);
            scissor.bottom = static_cast<LONG>(height);
        }

        D3D12_VERTEX_BUFFER_VIEW vertex_view{};
        vertex_view.BufferLocation = vertex_buffer->GetGPUVirtualAddress();
        vertex_view.SizeInBytes = static_cast<UINT>(vertex_bytes);
        vertex_view.StrideInBytes = sizeof(Rml::Vertex);

        D3D12_INDEX_BUFFER_VIEW index_view{};
        index_view.BufferLocation = index_buffer->GetGPUVirtualAddress();
        index_view.SizeInBytes = static_cast<UINT>(index_bytes);
        index_view.Format = DXGI_FORMAT_R32_UINT;

        ID3D12DescriptorHeap* descriptor_heaps[] = {srv_heap.Get()};
        command_list->SetDescriptorHeaps(1, descriptor_heaps);
        command_list->SetPipelineState(pipeline_state.Get());
        command_list->SetGraphicsRootSignature(root_signature.Get());
        const std::array<float, 2> viewport_constants{static_cast<float>(width), static_cast<float>(height)};
        command_list->SetGraphicsRoot32BitConstants(0, static_cast<UINT>(viewport_constants.size()), viewport_constants.data(), 0);
        command_list->SetGraphicsRootDescriptorTable(1, texture != nullptr ? texture->gpu_handle : white_texture->gpu_handle);
        command_list->RSSetViewports(1, &viewport);
        command_list->RSSetScissorRects(1, &scissor);
        command_list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        command_list->IASetVertexBuffers(0, 1, &vertex_view);
        command_list->IASetIndexBuffer(&index_view);
        command_list->DrawIndexedInstanced(static_cast<UINT>(geometry.indices.size()), 1, 0, 0, 0);

        frame_uploads[frame_index].push_back(std::move(vertex_buffer));
        frame_uploads[frame_index].push_back(std::move(index_buffer));
    }

    bool initialized = false;
    ID3D12Device* device = nullptr;
    ID3D12CommandQueue* command_queue = nullptr;
    ID3D12GraphicsCommandList* command_list = nullptr;
    std::uint32_t frame_index = 0;
    std::uint32_t width = 1;
    std::uint32_t height = 1;
    bool scissor_enabled = false;
    Rml::Rectanglei scissor_region = Rml::Rectanglei::MakeInvalid();

    std::uint32_t descriptor_size = 0;
    std::uint32_t next_texture_descriptor = 0;
    Texture* white_texture = nullptr;
    std::vector<std::unique_ptr<Texture>> textures;
    std::array<std::vector<ComPtr<ID3D12Resource>>, kFrameCount> frame_uploads;

    ComPtr<ID3D12DescriptorHeap> srv_heap;
    ComPtr<ID3D12RootSignature> root_signature;
    ComPtr<ID3D12PipelineState> pipeline_state;
    ComPtr<IWICImagingFactory> wic_factory;
    ComPtr<ID3D12CommandAllocator> upload_allocator;
    ComPtr<ID3D12GraphicsCommandList> upload_command_list;
    ComPtr<ID3D12Fence> upload_fence;
    HANDLE upload_fence_event = nullptr;
    std::uint64_t upload_fence_value = 1;
    std::uint64_t last_upload_signal = 0;
    bool com_initialized = false;
};

RmlDx12RenderInterface::RmlDx12RenderInterface()
    : impl_(std::make_unique<Impl>())
{
}

RmlDx12RenderInterface::~RmlDx12RenderInterface()
{
    shutdown();
}

bool RmlDx12RenderInterface::initialize(ID3D12Device* device, ID3D12CommandQueue* command_queue)
{
    return impl_->initialize(device, command_queue);
}

void RmlDx12RenderInterface::shutdown()
{
    if (impl_)
    {
        impl_->shutdown();
    }
}

void RmlDx12RenderInterface::begin_frame(ID3D12GraphicsCommandList* command_list, std::uint32_t frame_index, std::uint32_t width, std::uint32_t height)
{
    impl_->begin_frame(command_list, frame_index, width, height);
}

void RmlDx12RenderInterface::end_frame()
{
    impl_->end_frame();
}

Rml::CompiledGeometryHandle RmlDx12RenderInterface::CompileGeometry(Rml::Span<const Rml::Vertex> vertices, Rml::Span<const int> indices)
{
    auto* geometry = new Geometry;
    geometry->vertices.assign(vertices.begin(), vertices.end());
    geometry->indices.assign(indices.begin(), indices.end());
    return reinterpret_cast<Rml::CompiledGeometryHandle>(geometry);
}

void RmlDx12RenderInterface::RenderGeometry(Rml::CompiledGeometryHandle handle, Rml::Vector2f translation, Rml::TextureHandle texture)
{
    const auto* geometry = reinterpret_cast<const Geometry*>(handle);
    auto* texture_data = reinterpret_cast<Texture*>(texture);
    impl_->render_geometry(*geometry, translation, texture_data);
}

void RmlDx12RenderInterface::ReleaseGeometry(Rml::CompiledGeometryHandle geometry)
{
    delete reinterpret_cast<Geometry*>(geometry);
}

Rml::TextureHandle RmlDx12RenderInterface::LoadTexture(Rml::Vector2i& texture_dimensions, const Rml::String& source)
{
    texture_dimensions = {};
    const std::string normalized_source = normalize_texture_source(source);
    Texture* texture = is_video_texture_source(normalized_source)
        ? impl_->create_video_placeholder(normalized_source, texture_dimensions)
        : impl_->load_texture_file(normalized_source, texture_dimensions);
    return reinterpret_cast<Rml::TextureHandle>(texture);
}

Rml::TextureHandle RmlDx12RenderInterface::GenerateTexture(Rml::Span<const Rml::byte> source, Rml::Vector2i source_dimensions)
{
    if (source_dimensions.x <= 0 || source_dimensions.y <= 0 || source.empty())
    {
        return {};
    }

    Texture* texture = impl_->create_texture(source.data(), static_cast<std::uint32_t>(source_dimensions.x),
        static_cast<std::uint32_t>(source_dimensions.y), "RmlUi generated texture");
    return reinterpret_cast<Rml::TextureHandle>(texture);
}

void RmlDx12RenderInterface::ReleaseTexture(Rml::TextureHandle)
{
    // Texture descriptors are retained until renderer shutdown so in-flight frames remain valid.
}

void RmlDx12RenderInterface::EnableScissorRegion(bool enable)
{
    impl_->scissor_enabled = enable;
}

void RmlDx12RenderInterface::SetScissorRegion(Rml::Rectanglei region)
{
    impl_->scissor_region = region;
}
}

#endif
