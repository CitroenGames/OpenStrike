#pragma once

#include "openstrike/renderer/renderer.hpp"

#include <wrl/client.h>

#include <array>
#include <cstdint>
#include <memory>

namespace Rml
{
class Context;
}

class SystemInterface_SDL;
struct SDL_Window;
struct HWND__;
using HWND = HWND__*;
struct ID3D12CommandAllocator;
struct ID3D12CommandList;
struct ID3D12CommandQueue;
struct ID3D12DescriptorHeap;
struct ID3D12Device;
struct ID3D12Fence;
struct ID3D12GraphicsCommandList;
struct ID3D12PipelineState;
struct ID3D12Resource;
struct ID3D12RootSignature;
struct IDXGISwapChain3;
using HANDLE = void*;

namespace openstrike
{
class EngineContext;
class MainMenuController;
class RmlConsoleController;
class RmlDx12RenderInterface;
class RmlHudController;
struct LoadedWorld;

class Dx12Renderer final : public IRenderer
{
public:
    Dx12Renderer();
    ~Dx12Renderer() override;

    void set_engine_context(EngineContext* context) override;
    bool initialize(const RuntimeConfig& config) override;
    void render(const FrameContext& context) override;
    bool should_close() const override;
    void shutdown() override;

private:
    static constexpr std::uint32_t kFrameCount = 2;

    bool create_window(const RuntimeConfig& config);
    bool create_device_objects(const RuntimeConfig& config);
    bool create_render_targets();
    bool create_depth_buffer();
    bool create_world_pipeline();
    bool create_skybox_pipeline();
    bool resize_swap_chain(std::uint32_t width, std::uint32_t height);
    bool initialize_rml(const RuntimeConfig& config);
    bool load_rml_document(const RuntimeConfig& config);
    void sync_main_menu_visibility();
    void render_skybox(const LoadedWorld& world);
    void render_world();
    void shutdown_rml();
    void pump_messages();
    void wait_for_gpu();
    void move_to_next_frame();

    std::uint32_t width_ = 1280;
    std::uint32_t height_ = 720;
    bool initialized_ = false;
    bool window_closed_ = false;
    bool vsync_ = true;
    bool allow_tearing_ = false;
    bool sdl_initialized_ = false;
    bool rml_initialized_ = false;
    bool resize_pending_ = false;
    SDL_Window* window_ = nullptr;
    HWND hwnd_ = nullptr;
    HANDLE fence_event_ = nullptr;
    std::uint32_t frame_index_ = 0;
    std::uint32_t rtv_descriptor_size_ = 0;
    std::uint32_t dsv_descriptor_size_ = 0;
    std::uint64_t fence_value_ = 0;
    std::array<std::uint64_t, kFrameCount> frame_fence_values_{};

    Microsoft::WRL::ComPtr<ID3D12Device> device_;
    Microsoft::WRL::ComPtr<ID3D12CommandQueue> command_queue_;
    Microsoft::WRL::ComPtr<IDXGISwapChain3> swap_chain_;
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> rtv_heap_;
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> dsv_heap_;
    Microsoft::WRL::ComPtr<ID3D12RootSignature> skybox_root_signature_;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> skybox_pipeline_state_;
    Microsoft::WRL::ComPtr<ID3D12RootSignature> world_root_signature_;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> world_pipeline_state_;
    std::array<Microsoft::WRL::ComPtr<ID3D12Resource>, kFrameCount> render_targets_;
    Microsoft::WRL::ComPtr<ID3D12Resource> depth_buffer_;
    std::array<Microsoft::WRL::ComPtr<ID3D12CommandAllocator>, kFrameCount> command_allocators_;
    Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList> command_list_;
    Microsoft::WRL::ComPtr<ID3D12Fence> fence_;
    struct WorldGpuResources;
    struct SkyboxGpuResources;
    std::unique_ptr<WorldGpuResources> world_gpu_;
    std::unique_ptr<SkyboxGpuResources> skybox_gpu_;
    std::uint64_t world_gpu_generation_ = 0;
    std::uint64_t skybox_gpu_generation_ = 0;
    std::uint64_t main_menu_world_generation_ = 0;
    std::unique_ptr<SystemInterface_SDL> rml_system_interface_;
    std::unique_ptr<RmlDx12RenderInterface> rml_render_interface_;
    std::unique_ptr<MainMenuController> main_menu_controller_;
    std::unique_ptr<RmlHudController> rml_hud_controller_;
    std::unique_ptr<RmlConsoleController> rml_console_controller_;
    EngineContext* engine_context_ = nullptr;
    Rml::Context* rml_context_ = nullptr;
};
}
