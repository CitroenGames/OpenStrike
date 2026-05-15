#include "openstrike/renderer/world_material_shader.hpp"

namespace openstrike
{
Dx12ShaderFile world_material_vertex_shader_file()
{
    return {
        "world material vertex",
        "shaders/world_material.hlsl",
        "shaders/compiled/dx12/world_material.vs.cso",
        "VSMain",
        "vs_5_0",
    };
}

Dx12ShaderFile world_material_pixel_shader_file()
{
    return {
        "world material pixel",
        "shaders/world_material.hlsl",
        "shaders/compiled/dx12/world_material.ps.cso",
        "PSMain",
        "ps_5_0",
    };
}

Dx12ShaderFile skybox_vertex_shader_file()
{
    return {
        "skybox vertex",
        "shaders/skybox.hlsl",
        "shaders/compiled/dx12/skybox.vs.cso",
        "VSMain",
        "vs_5_0",
    };
}

Dx12ShaderFile skybox_pixel_shader_file()
{
    return {
        "skybox pixel",
        "shaders/skybox.hlsl",
        "shaders/compiled/dx12/skybox.ps.cso",
        "PSMain",
        "ps_5_0",
    };
}
}
