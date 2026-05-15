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
struct ID3D12Resource;
struct IDXGISwapChain3;
using HANDLE = void*;

namespace openstrike
{
class MainMenuController;
class RmlDx12RenderInterface;

class Dx12Renderer final : public IRenderer
{
public:
    Dx12Renderer();
    ~Dx12Renderer() override;

    bool initialize(const RuntimeConfig& config) override;
    void render(const FrameContext& context) override;
    bool should_close() const override;
    void shutdown() override;

private:
    static constexpr std::uint32_t kFrameCount = 2;

    bool create_window(const RuntimeConfig& config);
    bool create_device_objects(const RuntimeConfig& config);
    bool create_render_targets();
    bool resize_swap_chain(std::uint32_t width, std::uint32_t height);
    bool initialize_rml(const RuntimeConfig& config);
    bool load_rml_document(const RuntimeConfig& config);
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
    std::uint64_t fence_value_ = 0;
    std::array<std::uint64_t, kFrameCount> frame_fence_values_{};

    Microsoft::WRL::ComPtr<ID3D12Device> device_;
    Microsoft::WRL::ComPtr<ID3D12CommandQueue> command_queue_;
    Microsoft::WRL::ComPtr<IDXGISwapChain3> swap_chain_;
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> rtv_heap_;
    std::array<Microsoft::WRL::ComPtr<ID3D12Resource>, kFrameCount> render_targets_;
    std::array<Microsoft::WRL::ComPtr<ID3D12CommandAllocator>, kFrameCount> command_allocators_;
    Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList> command_list_;
    Microsoft::WRL::ComPtr<ID3D12Fence> fence_;
    std::unique_ptr<SystemInterface_SDL> rml_system_interface_;
    std::unique_ptr<RmlDx12RenderInterface> rml_render_interface_;
    std::unique_ptr<MainMenuController> main_menu_controller_;
    Rml::Context* rml_context_ = nullptr;
};
}
