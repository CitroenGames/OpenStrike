#include "openstrike/renderer/metal_renderer.hpp"

#include "openstrike/core/log.hpp"
#include "openstrike/engine/engine_context.hpp"

#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>
#include <SDL3/SDL_metal.h>

#import <Metal/Metal.h>
#import <QuartzCore/CAMetalLayer.h>

#include <algorithm>
#include <cstdint>

namespace openstrike
{
namespace
{
constexpr const char* kWindowTitle = "OpenStrike";

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

MTLClearColor frame_clear_color(std::uint64_t frame_index)
{
    const double pulse = static_cast<double>(frame_index % 180U) / 180.0;
    return MTLClearColorMake(0.025, 0.045 + (0.04 * pulse), 0.08 + (0.16 * pulse), 1.0);
}
}

struct MetalRenderer::Impl
{
    EngineContext* engine_context = nullptr;
    SDL_Window* window = nullptr;
    SDL_MetalView metal_view = nullptr;
    CAMetalLayer* layer = nil;
    id<MTLDevice> device = nil;
    id<MTLCommandQueue> command_queue = nil;
    id<MTLCommandBuffer> in_flight_command_buffer = nil;
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
        layer.pixelFormat = MTLPixelFormatBGRA8Unorm;
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

        SDL_ShowWindow(window);
        initialized = true;
        log_info("metal renderer initialized {}x{} vsync={} device={}", width, height, vsync ? "on" : "off", [[device name] UTF8String]);
        return true;
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

            id<MTLCommandBuffer> command_buffer = [command_queue commandBuffer];
            id<MTLRenderCommandEncoder> encoder = [command_buffer renderCommandEncoderWithDescriptor:pass_descriptor];
            [encoder endEncoding];
            [command_buffer presentDrawable:drawable];
            [command_buffer commit];
            in_flight_command_buffer = command_buffer;
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

        if (command_queue != nil)
        {
            command_queue = nil;
        }

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

            if (event.type == SDL_EVENT_KEY_DOWN && event.key.key == SDLK_ESCAPE && engine_context != nullptr &&
                engine_context->world.current_world() == nullptr)
            {
                window_closed = true;
                return;
            }

            if (engine_context != nullptr && (event.type == SDL_EVENT_KEY_DOWN || event.type == SDL_EVENT_KEY_UP))
            {
                if (set_gameplay_key(engine_context->input, event.key.key, event.type == SDL_EVENT_KEY_DOWN))
                {
                    continue;
                }
            }

            if (engine_context != nullptr && engine_context->input.mouse_captured && event.type == SDL_EVENT_MOUSE_MOTION)
            {
                engine_context->input.mouse_delta.x += event.motion.xrel;
                engine_context->input.mouse_delta.y += event.motion.yrel;
                continue;
            }
        }

        if (engine_context != nullptr)
        {
            const bool should_capture = engine_context->world.current_world() != nullptr;
            if (!should_capture)
            {
                engine_context->input.clear_gameplay_buttons();
            }

            if (mouse_captured != should_capture)
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
    return impl_->window_closed;
}

void MetalRenderer::shutdown()
{
    if (impl_)
    {
        impl_->shutdown();
    }
}
}
