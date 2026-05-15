#pragma once

namespace openstrike
{
struct Dx12ShaderFile
{
    const char* debug_name = "";
    const char* source_path = "";
    const char* compiled_path = "";
    const char* entry_point = "";
    const char* target = "";
};

[[nodiscard]] Dx12ShaderFile world_material_vertex_shader_file();
[[nodiscard]] Dx12ShaderFile world_material_pixel_shader_file();
[[nodiscard]] Dx12ShaderFile skybox_vertex_shader_file();
[[nodiscard]] Dx12ShaderFile skybox_pixel_shader_file();
}
