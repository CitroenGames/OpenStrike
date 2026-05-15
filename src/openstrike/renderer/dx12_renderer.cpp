#include "openstrike/renderer/dx12_renderer.hpp"

#include "openstrike/core/log.hpp"
#include "openstrike/engine/engine_context.hpp"
#include "openstrike/renderer/rml_dx12_render_interface.hpp"
#include "openstrike/ui/main_menu_controller.hpp"
#include "openstrike/ui/rml_console_controller.hpp"
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

#include <array>
#include <cassert>
#include <cctype>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <filesystem>
#include <format>
#include <limits>
#include <string>
#include <vector>

using Microsoft::WRL::ComPtr;

namespace openstrike
{
namespace
{
constexpr const char* kWindowTitle = "OpenStrike - DirectX 12";
constexpr DXGI_FORMAT kRenderTargetFormat = DXGI_FORMAT_R8G8B8A8_UNORM;

const char* kWorldVertexShader = R"(
cbuffer WorldConstants : register(b0)
{
    row_major float4x4 world_to_clip;
};

struct VSInput
{
    float3 position : POSITION;
    float3 normal : NORMAL;
};

struct PSInput
{
    float4 position : SV_POSITION;
    float3 normal : NORMAL;
};

PSInput main(VSInput input)
{
    PSInput output;
    output.position = mul(float4(input.position, 1.0f), world_to_clip);
    output.normal = input.normal;
    return output;
}
)";

const char* kWorldPixelShader = R"(
struct PSInput
{
    float4 position : SV_POSITION;
    float3 normal : NORMAL;
};

float4 main(PSInput input) : SV_TARGET
{
    const float up = saturate(abs(input.normal.z));
    const float side = saturate(abs(input.normal.x) * 0.35f + abs(input.normal.y) * 0.25f);
    const float3 floor_color = float3(0.48f, 0.52f, 0.47f);
    const float3 wall_color = float3(0.27f, 0.32f, 0.39f);
    const float3 color = lerp(wall_color, floor_color, up) + side;
    return float4(saturate(color), 1.0f);
}
)";

struct WorldDrawVertex
{
    float position[3]{};
    float normal[3]{};
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
            log_error("world shader compile error: {}", static_cast<const char*>(errors->GetBufferPointer()));
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

std::array<float, 16> world_to_clip_matrix(const WorldMesh& mesh, std::uint32_t width, std::uint32_t height)
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
    const float min_z = mesh.bounds_min.z - 64.0F;

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
        1.0F / (extent_z + 128.0F),
        0.0F,
        -center_x / half_width,
        center_y / half_height,
        -min_z / (extent_z + 128.0F),
        1.0F,
    };
}
}

struct Dx12Renderer::WorldGpuResources
{
    Microsoft::WRL::ComPtr<ID3D12Resource> vertex_buffer;
    Microsoft::WRL::ComPtr<ID3D12Resource> index_buffer;
    D3D12_VERTEX_BUFFER_VIEW vertex_view{};
    D3D12_INDEX_BUFFER_VIEW index_view{};
    std::uint32_t index_count = 0;
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

    if (!create_window(config) || !create_device_objects(config) || !create_world_pipeline() || !create_render_targets() || !initialize_rml(config))
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
    command_list_->OMSetRenderTargets(1, &rtv_handle, FALSE, nullptr);
    command_list_->ClearRenderTargetView(rtv_handle, clear_color.data(), 0, nullptr);

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
    command_allocators_ = {};
    world_gpu_.reset();
    world_pipeline_state_.Reset();
    world_root_signature_.Reset();
    command_list_.Reset();
    rtv_heap_.Reset();
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

bool Dx12Renderer::create_world_pipeline()
{
    D3D12_ROOT_PARAMETER root_parameter{};
    root_parameter.ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    root_parameter.ShaderVisibility = D3D12_SHADER_VISIBILITY_VERTEX;
    root_parameter.Constants.ShaderRegister = 0;
    root_parameter.Constants.RegisterSpace = 0;
    root_parameter.Constants.Num32BitValues = 16;

    D3D12_ROOT_SIGNATURE_DESC root_desc{};
    root_desc.NumParameters = 1;
    root_desc.pParameters = &root_parameter;
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

    ComPtr<ID3DBlob> vertex_shader = compile_shader(kWorldVertexShader, "main", "vs_5_0");
    ComPtr<ID3DBlob> pixel_shader = compile_shader(kWorldPixelShader, "main", "ps_5_0");
    if (!vertex_shader || !pixel_shader)
    {
        return false;
    }

    std::array<D3D12_INPUT_ELEMENT_DESC, 2> input_layout{{
        {"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, offsetof(WorldDrawVertex, position), D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
        {"NORMAL", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, offsetof(WorldDrawVertex, normal), D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
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
    pso_desc.DepthStencilState.DepthEnable = FALSE;
    pso_desc.DepthStencilState.StencilEnable = FALSE;
    pso_desc.SampleMask = std::numeric_limits<UINT>::max();
    pso_desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    pso_desc.NumRenderTargets = 1;
    pso_desc.RTVFormats[0] = kRenderTargetFormat;
    pso_desc.SampleDesc.Count = 1;

    return succeeded(device_->CreateGraphicsPipelineState(&pso_desc, IID_PPV_ARGS(&world_pipeline_state_)),
        "ID3D12Device::CreateGraphicsPipelineState world");
}

bool Dx12Renderer::resize_swap_chain(std::uint32_t width, std::uint32_t height)
{
    if (!swap_chain_ || width == 0 || height == 0)
    {
        return true;
    }

    wait_for_gpu();
    render_targets_ = {};

    const UINT flags = allow_tearing_ ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING : 0U;
    if (!succeeded(swap_chain_->ResizeBuffers(kFrameCount, width, height, DXGI_FORMAT_R8G8B8A8_UNORM, flags), "IDXGISwapChain::ResizeBuffers"))
    {
        return false;
    }

    frame_index_ = swap_chain_->GetCurrentBackBufferIndex();
    return create_render_targets();
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
    if (!load_rml_document(config))
    {
        return false;
    }

    if (engine_context_ != nullptr)
    {
        rml_console_controller_ = std::make_unique<RmlConsoleController>();
        if (!rml_console_controller_->initialize(*rml_context_, *engine_context_))
        {
            return false;
        }
    }

    return true;
}

bool Dx12Renderer::load_rml_document(const RuntimeConfig& config)
{
    const auto resolve_document = [&](const std::filesystem::path& document) {
        if (document.is_absolute())
        {
            return document;
        }
        return config.content_root / document;
    };

    std::vector<std::filesystem::path> candidates{
        resolve_document(config.rml_document),
        config.content_root / "assets/ui/mainmenu.rml",
        config.content_root / "csgo/resource/ui/mainmenu.rml",
        config.content_root / "csgo/resource/ui/rml/test.rml",
    };

    for (const std::filesystem::path& candidate : candidates)
    {
        std::error_code error;
        if (!std::filesystem::is_regular_file(candidate, error))
        {
            continue;
        }

        if (Rml::ElementDocument* document = rml_context_->LoadDocument(rml_path_string(candidate)))
        {
            document->Show();
            if (main_menu_controller_ != nullptr)
            {
                main_menu_controller_->attach(*document);
            }
            log_info("loaded RmlUi document '{}'", rml_path_string(candidate));
            return true;
        }
    }

    constexpr const char* fallback_document = R"(
<rml>
<head>
    <style>
        body { width: 100%; height: 100%; margin: 0; font-family: sans-serif; color: #f4f6f8; background-color: rgba(10, 12, 16, 235); }
        #panel { position: absolute; left: 48dp; top: 42dp; width: 420dp; padding: 22dp; background-color: rgba(30, 37, 48, 230); border-left-width: 4dp; border-left-color: #de9f37; }
        h1 { font-size: 28dp; margin-bottom: 8dp; color: #ffffff; }
        p { font-size: 14dp; color: #b8c1cf; }
    </style>
</head>
<body>
    <div id="panel">
        <h1>OpenStrike RmlUi</h1>
        <p>No external RML document was found. The DX12 renderer is running the fallback UI.</p>
    </div>
</body>
</rml>
)";

    if (Rml::ElementDocument* document = rml_context_->LoadDocumentFromMemory(fallback_document, "openstrike://fallback"))
    {
        document->Show();
        if (main_menu_controller_ != nullptr)
        {
            main_menu_controller_->attach(*document);
        }
        log_warning("using fallback RmlUi document; content root '{}' did not contain '{}'", rml_path_string(config.content_root),
            rml_path_string(config.rml_document));
        return true;
    }

    log_error("failed to load any RmlUi document");
    return false;
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

        world_gpu_ = std::move(gpu);
        world_gpu_generation_ = generation;
        log_info("uploaded world mesh '{}' vertices={} triangles={}", world->name, world->mesh.vertices.size(), world->mesh.indices.size() / 3);
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

    const std::array<float, 16> constants = world_to_clip_matrix(world->mesh, width_, height_);
    command_list_->SetPipelineState(world_pipeline_state_.Get());
    command_list_->SetGraphicsRootSignature(world_root_signature_.Get());
    command_list_->SetGraphicsRoot32BitConstants(0, static_cast<UINT>(constants.size()), constants.data(), 0);
    command_list_->RSSetViewports(1, &viewport);
    command_list_->RSSetScissorRects(1, &scissor);
    command_list_->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    command_list_->IASetVertexBuffers(0, 1, &world_gpu_->vertex_view);
    command_list_->IASetIndexBuffer(&world_gpu_->index_view);
    command_list_->DrawIndexedInstanced(world_gpu_->index_count, 1, 0, 0, 0);
}

void Dx12Renderer::shutdown_rml()
{
    rml_console_controller_.reset();
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

        if (rml_context_ != nullptr)
        {
            RmlSDL::InputEventHandler(rml_context_, window_, event);
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
