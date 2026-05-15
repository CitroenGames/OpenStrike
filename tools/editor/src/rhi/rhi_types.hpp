#pragma once

#include <cstdint>
#include <imgui.h>

// Opaque GPU resource handles — internally cast to backend-specific types.
using RhiBuffer       = uint64_t;  // VBO / IBO
using RhiVertexLayout = uint64_t;  // VAO / Vulkan vertex binding ref
using RhiTexture      = uint64_t;  // GL texture / VkImage+View+Sampler
using RhiFramebuffer  = uint64_t;  // FBO / Vulkan offscreen framebuffer
using RhiShader       = uint64_t;  // GL program / Vulkan pipeline

constexpr RhiBuffer       RHI_NULL_BUFFER       = 0;
constexpr RhiVertexLayout RHI_NULL_VERTEX_LAYOUT = 0;
constexpr RhiTexture      RHI_NULL_TEXTURE      = 0;
constexpr RhiFramebuffer  RHI_NULL_FRAMEBUFFER  = 0;
constexpr RhiShader       RHI_NULL_SHADER       = 0;

enum class RhiBufferUsage { Vertex, Index };

enum class RhiPrimitive { Triangles, Lines };

struct RhiVertexAttrib
{
    int location;     // shader attribute location
    int components;   // 1..4
    int strideBytes;
    int offsetBytes;
};
