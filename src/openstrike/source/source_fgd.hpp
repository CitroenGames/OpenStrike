#pragma once

#include "openstrike/core/math.hpp"

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace openstrike
{
enum class SourceFgdClassKind
{
    Base,
    Point,
    Solid,
    Move,
    KeyFrame,
    Npc,
    Filter
};

enum class SourceFgdValueType
{
    Bad,
    Angle,
    TargetDestination,
    TargetNameOrClass,
    TargetSource,
    Integer,
    String,
    StringInstanced,
    Choices,
    Flags,
    Decal,
    Color255,
    Color1,
    StudioModel,
    Sprite,
    Sound,
    Vector,
    NpcClass,
    FilterClass,
    Float,
    Material,
    Scene,
    Side,
    SideList,
    Origin,
    VecLine,
    Axis,
    PointEntityClass,
    NodeDestination,
    Script,
    ScriptList,
    ParticleSystem,
    InstanceFile,
    AngleNegativePitch,
    InstanceVariable,
    InstanceParameter,
    Boolean,
    NodeId
};

enum class SourceFgdIoType
{
    Invalid,
    Void,
    Integer,
    Boolean,
    String,
    Float,
    Vector,
    EntityHandle,
    Color,
    Script
};

struct SourceFgdColor
{
    std::uint8_t r = 220;
    std::uint8_t g = 30;
    std::uint8_t b = 220;
    std::uint8_t a = 0;
};

struct SourceFgdHelper
{
    std::string name;
    std::vector<std::string> parameters;
};

struct SourceFgdChoice
{
    std::string value;
    std::string caption;
    std::uint32_t flag_value = 0;
    bool default_enabled = false;
};

struct SourceFgdVariable
{
    std::string name;
    std::string display_name;
    std::string description;
    SourceFgdValueType type = SourceFgdValueType::Bad;
    std::string default_value;
    int default_integer = 0;
    bool reportable = false;
    bool read_only = false;
    std::vector<SourceFgdChoice> choices;
};

struct SourceFgdInputOutput
{
    std::string name;
    SourceFgdIoType type = SourceFgdIoType::Invalid;
    std::string description;
};

struct SourceFgdEntityClass
{
    std::string name;
    std::string description;
    SourceFgdClassKind kind = SourceFgdClassKind::Point;
    std::vector<std::string> bases;
    std::vector<SourceFgdVariable> variables;
    std::vector<SourceFgdInputOutput> inputs;
    std::vector<SourceFgdInputOutput> outputs;
    std::vector<SourceFgdHelper> helpers;
    Vec3 mins{-8.0F, -8.0F, -8.0F};
    Vec3 maxs{8.0F, 8.0F, 8.0F};
    SourceFgdColor color;
    bool has_size = false;
    bool has_color = false;
    bool half_grid_snap = false;
};

struct SourceFgdGridNav
{
    int edge_size = 0;
    int offset_x = 0;
    int offset_y = 0;
    int trace_height = 0;
};

struct SourceFgdMaterialExclusion
{
    std::string directory;
    bool user_generated = false;
};

struct SourceFgdAutoVisGroupClass
{
    std::string name;
    std::vector<std::string> entities;
};

struct SourceFgdAutoVisGroup
{
    std::string parent;
    std::vector<SourceFgdAutoVisGroupClass> classes;
};

class SourceFgdGameData
{
public:
    bool load_file(const std::filesystem::path& path);
    bool load_text(std::string_view text, std::filesystem::path source_path = {});
    void clear();

    [[nodiscard]] const SourceFgdEntityClass* find_class(std::string_view class_name) const;
    [[nodiscard]] const std::vector<SourceFgdEntityClass>& classes() const;
    [[nodiscard]] const std::vector<SourceFgdMaterialExclusion>& material_exclusions() const;
    [[nodiscard]] const std::vector<SourceFgdAutoVisGroup>& auto_vis_groups() const;
    [[nodiscard]] const std::vector<std::string>& errors() const;

    [[nodiscard]] int min_map_coord() const;
    [[nodiscard]] int max_map_coord() const;
    [[nodiscard]] std::optional<SourceFgdGridNav> grid_nav() const;

private:
    friend class SourceFgdParser;

    bool load_file_internal(const std::filesystem::path& path);
    void upsert_class(SourceFgdEntityClass entity_class);

    std::vector<SourceFgdEntityClass> classes_;
    std::vector<SourceFgdMaterialExclusion> material_exclusions_;
    std::vector<SourceFgdAutoVisGroup> auto_vis_groups_;
    std::vector<std::string> errors_;
    std::vector<std::filesystem::path> include_stack_;
    int min_map_coord_ = -8192;
    int max_map_coord_ = 8192;
    std::optional<SourceFgdGridNav> grid_nav_;
};

[[nodiscard]] SourceFgdValueType source_fgd_value_type_from_string(std::string_view text);
[[nodiscard]] std::string_view to_string(SourceFgdValueType type);
[[nodiscard]] SourceFgdIoType source_fgd_io_type_from_string(std::string_view text);
[[nodiscard]] std::string_view to_string(SourceFgdIoType type);
[[nodiscard]] std::string_view to_string(SourceFgdClassKind kind);
}
