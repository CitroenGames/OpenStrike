#include "openstrike/source/source_fgd.hpp"

#include <algorithm>
#include <charconv>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <system_error>
#include <utility>

namespace openstrike
{
namespace
{
enum class FgdTokenKind
{
    End,
    Identifier,
    String,
    Number,
    Operator
};

struct FgdToken
{
    FgdTokenKind kind = FgdTokenKind::End;
    std::string text;
    std::size_t line = 1;
    std::size_t column = 1;
};

struct ValueTypeMapEntry
{
    SourceFgdValueType type;
    std::string_view name;
    bool stored_as_integer;
};

constexpr ValueTypeMapEntry kValueTypes[] = {
    {SourceFgdValueType::Angle, "angle", false},
    {SourceFgdValueType::Choices, "choices", false},
    {SourceFgdValueType::Color1, "color1", false},
    {SourceFgdValueType::Color255, "color255", false},
    {SourceFgdValueType::Decal, "decal", false},
    {SourceFgdValueType::Flags, "flags", true},
    {SourceFgdValueType::Integer, "integer", true},
    {SourceFgdValueType::Sound, "sound", false},
    {SourceFgdValueType::Sprite, "sprite", false},
    {SourceFgdValueType::String, "string", false},
    {SourceFgdValueType::StringInstanced, "string_instanced", false},
    {SourceFgdValueType::StudioModel, "studio", false},
    {SourceFgdValueType::TargetDestination, "target_destination", false},
    {SourceFgdValueType::TargetSource, "target_source", false},
    {SourceFgdValueType::TargetNameOrClass, "target_name_or_class", false},
    {SourceFgdValueType::Vector, "vector", false},
    {SourceFgdValueType::NpcClass, "npcclass", false},
    {SourceFgdValueType::FilterClass, "filterclass", false},
    {SourceFgdValueType::Float, "float", false},
    {SourceFgdValueType::Material, "material", false},
    {SourceFgdValueType::Scene, "scene", false},
    {SourceFgdValueType::Side, "side", false},
    {SourceFgdValueType::SideList, "sidelist", false},
    {SourceFgdValueType::Origin, "origin", false},
    {SourceFgdValueType::Axis, "axis", false},
    {SourceFgdValueType::VecLine, "vecline", false},
    {SourceFgdValueType::PointEntityClass, "pointentityclass", false},
    {SourceFgdValueType::NodeDestination, "node_dest", true},
    {SourceFgdValueType::Script, "script", false},
    {SourceFgdValueType::ScriptList, "scriptlist", false},
    {SourceFgdValueType::ParticleSystem, "particlesystem", false},
    {SourceFgdValueType::InstanceFile, "instance_file", false},
    {SourceFgdValueType::AngleNegativePitch, "angle_negative_pitch", false},
    {SourceFgdValueType::InstanceVariable, "instance_variable", false},
    {SourceFgdValueType::InstanceParameter, "instance_parm", false},
    {SourceFgdValueType::Boolean, "boolean", false},
    {SourceFgdValueType::NodeId, "node_id", true},
};

struct IoTypeMapEntry
{
    SourceFgdIoType type;
    std::string_view name;
};

constexpr IoTypeMapEntry kIoTypes[] = {
    {SourceFgdIoType::Void, "void"},
    {SourceFgdIoType::Integer, "integer"},
    {SourceFgdIoType::Boolean, "bool"},
    {SourceFgdIoType::String, "string"},
    {SourceFgdIoType::Float, "float"},
    {SourceFgdIoType::Vector, "vector"},
    {SourceFgdIoType::EntityHandle, "target_destination"},
    {SourceFgdIoType::Color, "color255"},
    {SourceFgdIoType::EntityHandle, "ehandle"},
    {SourceFgdIoType::Script, "script"},
};

std::string lower_copy(std::string_view text)
{
    std::string result(text);
    std::transform(result.begin(), result.end(), result.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    return result;
}

bool equals_icase(std::string_view lhs, std::string_view rhs)
{
    return lower_copy(lhs) == lower_copy(rhs);
}

bool is_operator_char(char ch)
{
    switch (ch)
    {
    case '@':
    case '(':
    case ')':
    case '[':
    case ']':
    case '=':
    case ',':
    case ':':
    case '*':
        return true;
    default:
        return false;
    }
}

std::optional<int> parse_int(std::string_view text)
{
    int value = 0;
    const char* begin = text.data();
    const char* end = text.data() + text.size();
    const auto result = std::from_chars(begin, end, value);
    if (result.ec != std::errc{} || result.ptr != end)
    {
        return std::nullopt;
    }

    return value;
}

std::optional<float> parse_float(std::string_view text)
{
    std::string value_text(text);
    char* end = nullptr;
    const float value = std::strtof(value_text.c_str(), &end);
    if (end == value_text.c_str() || *end != '\0' || !std::isfinite(value))
    {
        return std::nullopt;
    }

    return value;
}

bool value_type_stores_integer(SourceFgdValueType type)
{
    for (const ValueTypeMapEntry& entry : kValueTypes)
    {
        if (entry.type == type)
        {
            return entry.stored_as_integer;
        }
    }

    return false;
}

std::string read_text_file(const std::filesystem::path& path)
{
    std::ifstream file(path, std::ios::binary);
    if (!file)
    {
        throw std::runtime_error("failed to open file: " + path.string());
    }

    std::ostringstream contents;
    contents << file.rdbuf();
    return contents.str();
}

bool file_exists(const std::filesystem::path& path)
{
    std::error_code error;
    return std::filesystem::is_regular_file(path, error);
}

std::filesystem::path normalized_absolute(const std::filesystem::path& path)
{
    std::error_code error;
    std::filesystem::path absolute = std::filesystem::absolute(path, error);
    if (error)
    {
        absolute = path;
    }

    return absolute.lexically_normal();
}

void merge_missing_choices(SourceFgdVariable& target, const SourceFgdVariable& source)
{
    for (const SourceFgdChoice& source_choice : source.choices)
    {
        const auto it = std::find_if(target.choices.begin(), target.choices.end(), [&](const SourceFgdChoice& target_choice) {
            if (target.type == SourceFgdValueType::Flags)
            {
                return target_choice.flag_value == source_choice.flag_value;
            }

            return equals_icase(target_choice.value, source_choice.value);
        });

        if (it == target.choices.end())
        {
            target.choices.push_back(source_choice);
        }
    }
}

void upsert_variable(SourceFgdEntityClass& entity_class, SourceFgdVariable variable)
{
    const auto it = std::find_if(entity_class.variables.begin(), entity_class.variables.end(), [&](const SourceFgdVariable& existing) {
        return equals_icase(existing.name, variable.name);
    });

    if (it == entity_class.variables.end())
    {
        entity_class.variables.push_back(std::move(variable));
        return;
    }

    if (it->type == variable.type && (variable.type == SourceFgdValueType::Flags || variable.type == SourceFgdValueType::Choices))
    {
        merge_missing_choices(variable, *it);
    }

    *it = std::move(variable);
}

void append_base(SourceFgdEntityClass& entity_class, const SourceFgdEntityClass& base)
{
    entity_class.bases.push_back(base.name);
    for (const SourceFgdVariable& variable : base.variables)
    {
        upsert_variable(entity_class, variable);
    }

    entity_class.inputs.insert(entity_class.inputs.end(), base.inputs.begin(), base.inputs.end());
    entity_class.outputs.insert(entity_class.outputs.end(), base.outputs.begin(), base.outputs.end());

    if (!entity_class.has_size && base.has_size)
    {
        entity_class.mins = base.mins;
        entity_class.maxs = base.maxs;
        entity_class.has_size = true;
    }

    if (!entity_class.has_color && base.has_color)
    {
        entity_class.color = base.color;
        entity_class.has_color = true;
    }
}

class FgdTokenizer
{
public:
    explicit FgdTokenizer(std::string_view text)
        : text_(text)
    {
    }

    FgdToken next()
    {
        if (cached_)
        {
            FgdToken token = std::move(*cached_);
            cached_.reset();
            return token;
        }

        return read_token();
    }

    FgdToken peek()
    {
        if (!cached_)
        {
            cached_ = read_token();
        }

        return *cached_;
    }

    void unread(FgdToken token)
    {
        cached_ = std::move(token);
    }

private:
    void advance()
    {
        if (cursor_ >= text_.size())
        {
            return;
        }

        if (text_[cursor_] == '\n')
        {
            ++line_;
            column_ = 1;
        }
        else
        {
            ++column_;
        }

        ++cursor_;
    }

    void skip_ignored()
    {
        while (cursor_ < text_.size())
        {
            const char ch = text_[cursor_];
            if (std::isspace(static_cast<unsigned char>(ch)) != 0)
            {
                advance();
                continue;
            }

            if (ch == '/' && cursor_ + 1 < text_.size() && text_[cursor_ + 1] == '/')
            {
                while (cursor_ < text_.size() && text_[cursor_] != '\n')
                {
                    advance();
                }
                continue;
            }

            if (ch == '/' && cursor_ + 1 < text_.size() && text_[cursor_ + 1] == '*')
            {
                advance();
                advance();
                while (cursor_ + 1 < text_.size())
                {
                    if (text_[cursor_] == '*' && text_[cursor_ + 1] == '/')
                    {
                        advance();
                        advance();
                        break;
                    }
                    advance();
                }
                continue;
            }

            break;
        }
    }

    FgdToken read_quoted(std::size_t line, std::size_t column)
    {
        advance();
        std::string value;
        while (cursor_ < text_.size())
        {
            const char ch = text_[cursor_];
            advance();
            if (ch == '"')
            {
                break;
            }

            if (ch == '\\' && cursor_ < text_.size())
            {
                const char escaped = text_[cursor_];
                advance();
                switch (escaped)
                {
                case 'n':
                    value.push_back('\n');
                    break;
                case 'r':
                    value.push_back('\r');
                    break;
                case 't':
                    value.push_back('\t');
                    break;
                default:
                    value.push_back(escaped);
                    break;
                }
                continue;
            }

            value.push_back(ch);
        }

        return FgdToken{.kind = FgdTokenKind::String, .text = std::move(value), .line = line, .column = column};
    }

    FgdToken read_number(std::size_t line, std::size_t column)
    {
        const std::size_t begin = cursor_;
        if (text_[cursor_] == '-' || text_[cursor_] == '+')
        {
            advance();
        }

        while (cursor_ < text_.size() && std::isdigit(static_cast<unsigned char>(text_[cursor_])) != 0)
        {
            advance();
        }

        if (cursor_ < text_.size() && text_[cursor_] == '.')
        {
            advance();
            while (cursor_ < text_.size() && std::isdigit(static_cast<unsigned char>(text_[cursor_])) != 0)
            {
                advance();
            }
        }

        return FgdToken{
            .kind = FgdTokenKind::Number,
            .text = std::string(text_.substr(begin, cursor_ - begin)),
            .line = line,
            .column = column,
        };
    }

    FgdToken read_identifier(std::size_t line, std::size_t column)
    {
        const std::size_t begin = cursor_;
        while (cursor_ < text_.size())
        {
            const char ch = text_[cursor_];
            if (std::isspace(static_cast<unsigned char>(ch)) != 0 || is_operator_char(ch) || ch == '"')
            {
                break;
            }

            if (ch == '/' && cursor_ + 1 < text_.size() && (text_[cursor_ + 1] == '/' || text_[cursor_ + 1] == '*'))
            {
                break;
            }

            advance();
        }

        return FgdToken{
            .kind = FgdTokenKind::Identifier,
            .text = std::string(text_.substr(begin, cursor_ - begin)),
            .line = line,
            .column = column,
        };
    }

    FgdToken read_token()
    {
        skip_ignored();
        const std::size_t line = line_;
        const std::size_t column = column_;
        if (cursor_ >= text_.size())
        {
            return FgdToken{.kind = FgdTokenKind::End, .line = line, .column = column};
        }

        const char ch = text_[cursor_];
        if (ch == '"')
        {
            return read_quoted(line, column);
        }

        if (is_operator_char(ch))
        {
            advance();
            return FgdToken{.kind = FgdTokenKind::Operator, .text = std::string(1, ch), .line = line, .column = column};
        }

        const bool signed_number =
            (ch == '-' || ch == '+') && cursor_ + 1 < text_.size() && std::isdigit(static_cast<unsigned char>(text_[cursor_ + 1])) != 0;
        if (std::isdigit(static_cast<unsigned char>(ch)) != 0 || signed_number)
        {
            return read_number(line, column);
        }

        return read_identifier(line, column);
    }

    std::string_view text_;
    std::size_t cursor_ = 0;
    std::size_t line_ = 1;
    std::size_t column_ = 1;
    std::optional<FgdToken> cached_;
};
}

class SourceFgdParser
{
public:
    SourceFgdParser(SourceFgdGameData& game_data, std::string_view text, std::filesystem::path source_path)
        : game_data_(game_data)
        , tokenizer_(text)
        , source_path_(std::move(source_path))
    {
    }

    bool parse()
    {
        while (true)
        {
            const FgdToken token = tokenizer_.next();
            if (token.kind == FgdTokenKind::End)
            {
                break;
            }

            if (!is_operator(token, "@"))
            {
                add_error(token, "expected '@'");
                skip_until_next_section();
                continue;
            }

            std::string section;
            if (!consume_identifier(section, "section name"))
            {
                skip_until_next_section();
                continue;
            }

            if (is_class_section(section))
            {
                parse_class(section);
            }
            else if (equals_icase(section, "include"))
            {
                parse_include();
            }
            else if (equals_icase(section, "mapsize"))
            {
                parse_map_size();
            }
            else if (equals_icase(section, "gridnav"))
            {
                parse_grid_nav();
            }
            else if (equals_icase(section, "materialexclusion"))
            {
                parse_material_exclusion();
            }
            else if (equals_icase(section, "autovisgroup"))
            {
                parse_auto_vis_group();
            }
            else
            {
                add_error(token, "unrecognized FGD section '" + section + "'");
                skip_until_next_section();
            }
        }

        return game_data_.errors_.empty();
    }

private:
    static bool is_operator(const FgdToken& token, std::string_view text)
    {
        return token.kind == FgdTokenKind::Operator && token.text == text;
    }

    static bool is_value_token(const FgdToken& token)
    {
        return token.kind == FgdTokenKind::Identifier || token.kind == FgdTokenKind::String || token.kind == FgdTokenKind::Number;
    }

    static bool is_class_section(std::string_view section)
    {
        return equals_icase(section, "baseclass") || equals_icase(section, "pointclass") || equals_icase(section, "solidclass") ||
               equals_icase(section, "keyframeclass") || equals_icase(section, "moveclass") || equals_icase(section, "npcclass") ||
               equals_icase(section, "filterclass");
    }

    static SourceFgdClassKind class_kind_from_section(std::string_view section)
    {
        if (equals_icase(section, "baseclass"))
        {
            return SourceFgdClassKind::Base;
        }
        if (equals_icase(section, "solidclass"))
        {
            return SourceFgdClassKind::Solid;
        }
        if (equals_icase(section, "moveclass"))
        {
            return SourceFgdClassKind::Move;
        }
        if (equals_icase(section, "keyframeclass"))
        {
            return SourceFgdClassKind::KeyFrame;
        }
        if (equals_icase(section, "npcclass"))
        {
            return SourceFgdClassKind::Npc;
        }
        if (equals_icase(section, "filterclass"))
        {
            return SourceFgdClassKind::Filter;
        }
        return SourceFgdClassKind::Point;
    }

    void add_error(const FgdToken& token, std::string message)
    {
        std::ostringstream formatted;
        if (!source_path_.empty())
        {
            formatted << source_path_.string() << ':';
        }
        formatted << token.line << ':' << token.column << ": " << message;
        game_data_.errors_.push_back(formatted.str());
    }

    bool consume_operator(std::string_view text)
    {
        const FgdToken token = tokenizer_.next();
        if (!is_operator(token, text))
        {
            add_error(token, "expected '" + std::string(text) + "'");
            return false;
        }
        return true;
    }

    bool consume_identifier(std::string& out, std::string_view expectation)
    {
        const FgdToken token = tokenizer_.next();
        if (token.kind != FgdTokenKind::Identifier)
        {
            add_error(token, "expected " + std::string(expectation));
            return false;
        }

        out = token.text;
        return true;
    }

    bool consume_value(std::string& out, std::string_view expectation)
    {
        const FgdToken token = tokenizer_.next();
        if (!is_value_token(token))
        {
            add_error(token, "expected " + std::string(expectation));
            return false;
        }

        out = token.text;
        return true;
    }

    bool consume_int(int& out, std::string_view expectation)
    {
        std::string token_text;
        if (!consume_value(token_text, expectation))
        {
            return false;
        }

        const std::optional<int> value = parse_int(token_text);
        if (!value)
        {
            add_error(tokenizer_.peek(), "expected integer for " + std::string(expectation));
            return false;
        }

        out = *value;
        return true;
    }

    bool consume_float(float& out, std::string_view expectation)
    {
        std::string token_text;
        if (!consume_value(token_text, expectation))
        {
            return false;
        }

        const std::optional<float> value = parse_float(token_text);
        if (!value)
        {
            add_error(tokenizer_.peek(), "expected number for " + std::string(expectation));
            return false;
        }

        out = *value;
        return true;
    }

    void skip_until_next_section()
    {
        while (true)
        {
            FgdToken token = tokenizer_.next();
            if (token.kind == FgdTokenKind::End)
            {
                return;
            }

            if (is_operator(token, "@"))
            {
                tokenizer_.unread(std::move(token));
                return;
            }
        }
    }

    void parse_include()
    {
        std::string include_path_text;
        if (!consume_value(include_path_text, "include path"))
        {
            return;
        }

        std::filesystem::path include_path(include_path_text);
        std::vector<std::filesystem::path> candidates;
        if (include_path.is_absolute())
        {
            candidates.push_back(include_path);
        }
        else
        {
            if (!source_path_.empty())
            {
                candidates.push_back(source_path_.parent_path() / include_path);
            }
            candidates.push_back(include_path);
        }

        for (const std::filesystem::path& candidate : candidates)
        {
            if (file_exists(candidate))
            {
                game_data_.load_file_internal(candidate);
                return;
            }
        }

        add_error(tokenizer_.peek(), "error including file '" + include_path_text + "'");
    }

    bool parse_vec3(Vec3& out)
    {
        return consume_float(out.x, "x coordinate") && consume_float(out.y, "y coordinate") && consume_float(out.z, "z coordinate");
    }

    bool parse_base_specifier(SourceFgdEntityClass& entity_class)
    {
        while (true)
        {
            std::string base_name;
            if (!consume_identifier(base_name, "base class name"))
            {
                return false;
            }

            const SourceFgdEntityClass* base = game_data_.find_class(base_name);
            if (base == nullptr)
            {
                add_error(tokenizer_.peek(), "undefined base class '" + base_name + "'");
                return false;
            }

            append_base(entity_class, *base);

            const FgdToken token = tokenizer_.next();
            if (is_operator(token, ")"))
            {
                return true;
            }
            if (!is_operator(token, ","))
            {
                add_error(token, "expected ',' or ')'");
                return false;
            }
        }
    }

    bool parse_size_specifier(SourceFgdEntityClass& entity_class)
    {
        Vec3 mins;
        if (!parse_vec3(mins))
        {
            return false;
        }

        Vec3 maxs;
        if (is_operator(tokenizer_.peek(), ","))
        {
            tokenizer_.next();
            if (!parse_vec3(maxs))
            {
                return false;
            }
        }
        else
        {
            maxs = mins * 0.5F;
            mins = maxs * -1.0F;
        }

        if (!consume_operator(")"))
        {
            return false;
        }

        entity_class.mins = mins;
        entity_class.maxs = maxs;
        entity_class.has_size = true;
        return true;
    }

    bool parse_color_specifier(SourceFgdEntityClass& entity_class)
    {
        int r = 0;
        int g = 0;
        int b = 0;
        if (!consume_int(r, "red color component") || !consume_int(g, "green color component") || !consume_int(b, "blue color component"))
        {
            return false;
        }

        if (!consume_operator(")"))
        {
            return false;
        }

        entity_class.color.r = static_cast<std::uint8_t>(std::clamp(r, 0, 255));
        entity_class.color.g = static_cast<std::uint8_t>(std::clamp(g, 0, 255));
        entity_class.color.b = static_cast<std::uint8_t>(std::clamp(b, 0, 255));
        entity_class.color.a = 0;
        entity_class.has_color = true;
        return true;
    }

    bool parse_helper_specifier(SourceFgdEntityClass& entity_class, std::string helper_name)
    {
        SourceFgdHelper helper;
        helper.name = std::move(helper_name);

        while (true)
        {
            const FgdToken token = tokenizer_.next();
            if (token.kind == FgdTokenKind::End)
            {
                add_error(token, "expected ')'");
                return false;
            }
            if (is_operator(token, ")"))
            {
                entity_class.helpers.push_back(std::move(helper));
                return true;
            }
            if (is_operator(token, ","))
            {
                continue;
            }
            if (is_operator(token, "="))
            {
                add_error(token, "unexpected '=' in helper specifier");
                return false;
            }
            if (is_value_token(token))
            {
                helper.parameters.push_back(token.text);
                continue;
            }

            add_error(token, "unexpected token in helper specifier");
            return false;
        }
    }

    bool parse_specifiers(SourceFgdEntityClass& entity_class)
    {
        while (tokenizer_.peek().kind == FgdTokenKind::Identifier)
        {
            std::string specifier = tokenizer_.next().text;
            if (equals_icase(specifier, "halfgridsnap"))
            {
                entity_class.half_grid_snap = true;
                continue;
            }

            if (!consume_operator("("))
            {
                return false;
            }

            if (equals_icase(specifier, "base"))
            {
                if (!parse_base_specifier(entity_class))
                {
                    return false;
                }
            }
            else if (equals_icase(specifier, "size"))
            {
                if (!parse_size_specifier(entity_class))
                {
                    return false;
                }
            }
            else if (equals_icase(specifier, "color"))
            {
                if (!parse_color_specifier(entity_class))
                {
                    return false;
                }
            }
            else if (!parse_helper_specifier(entity_class, std::move(specifier)))
            {
                return false;
            }
        }

        return true;
    }

    bool parse_input_output(SourceFgdInputOutput& io)
    {
        if (!consume_identifier(io.name, "input/output name"))
        {
            return false;
        }

        if (!consume_operator("("))
        {
            return false;
        }

        std::string type_name;
        if (!consume_identifier(type_name, "input/output type"))
        {
            return false;
        }

        io.type = source_fgd_io_type_from_string(type_name);
        if (io.type == SourceFgdIoType::Invalid)
        {
            add_error(tokenizer_.peek(), "bad input/output type '" + type_name + "'");
            return false;
        }

        if (!consume_operator(")"))
        {
            return false;
        }

        if (is_operator(tokenizer_.peek(), ":"))
        {
            tokenizer_.next();
            if (!consume_value(io.description, "input/output description"))
            {
                return false;
            }
        }

        return true;
    }

    bool parse_choices(SourceFgdVariable& variable)
    {
        if (!consume_operator("=") || !consume_operator("["))
        {
            return false;
        }

        if (variable.type == SourceFgdValueType::Flags)
        {
            std::uint32_t default_flags = 0;
            while (tokenizer_.peek().kind == FgdTokenKind::Number || tokenizer_.peek().kind == FgdTokenKind::Identifier)
            {
                std::string flag_value_text;
                if (!consume_value(flag_value_text, "flag value"))
                {
                    return false;
                }

                const std::optional<int> flag_value = parse_int(flag_value_text);
                if (!flag_value)
                {
                    add_error(tokenizer_.peek(), "expected integer flag value");
                    return false;
                }

                if (!consume_operator(":"))
                {
                    return false;
                }

                SourceFgdChoice choice;
                choice.flag_value = static_cast<std::uint32_t>(*flag_value);
                choice.value = std::to_string(choice.flag_value);
                if (!consume_value(choice.caption, "flag caption"))
                {
                    return false;
                }

                if (!consume_operator(":"))
                {
                    return false;
                }

                int default_enabled = 0;
                if (!consume_int(default_enabled, "flag default"))
                {
                    return false;
                }

                choice.default_enabled = default_enabled != 0;
                if (choice.default_enabled)
                {
                    default_flags |= choice.flag_value;
                }
                variable.choices.push_back(std::move(choice));
            }

            variable.default_integer = static_cast<int>(default_flags);
            variable.default_value = std::to_string(default_flags);
        }
        else
        {
            while (is_value_token(tokenizer_.peek()))
            {
                SourceFgdChoice choice;
                if (!consume_value(choice.value, "choice value"))
                {
                    return false;
                }

                if (!consume_operator(":"))
                {
                    return false;
                }

                if (!consume_value(choice.caption, "choice caption"))
                {
                    return false;
                }

                variable.choices.push_back(std::move(choice));
            }
        }

        return consume_operator("]");
    }

    bool finalize_boolean_variable(SourceFgdVariable& variable)
    {
        if (variable.type != SourceFgdValueType::Boolean)
        {
            return true;
        }

        variable.type = SourceFgdValueType::Choices;
        if (equals_icase(variable.default_value, "yes"))
        {
            variable.default_value = "1";
        }
        else if (equals_icase(variable.default_value, "no") || variable.default_value.empty())
        {
            variable.default_value = "0";
        }

        const std::optional<int> default_value = parse_int(variable.default_value);
        if (!default_value || (*default_value != 0 && *default_value != 1))
        {
            add_error(tokenizer_.peek(), "boolean type specified with nonsensical default value: " + variable.default_value);
            return false;
        }

        variable.default_integer = *default_value;
        variable.choices.push_back(SourceFgdChoice{.value = "1", .caption = "Yes"});
        variable.choices.push_back(SourceFgdChoice{.value = "0", .caption = "No"});
        return true;
    }

    bool parse_variable(SourceFgdVariable& variable)
    {
        if (!consume_identifier(variable.name, "variable name"))
        {
            return false;
        }

        if (!consume_operator("("))
        {
            return false;
        }

        if (is_operator(tokenizer_.peek(), "*"))
        {
            tokenizer_.next();
            variable.reportable = true;
        }

        std::string type_name;
        if (!consume_identifier(type_name, "variable type"))
        {
            return false;
        }

        variable.type = source_fgd_value_type_from_string(type_name);
        if (variable.type == SourceFgdValueType::Bad)
        {
            add_error(tokenizer_.peek(), "'" + type_name + "' is not a valid variable type");
            return false;
        }

        if (!consume_operator(")"))
        {
            return false;
        }

        while (tokenizer_.peek().kind == FgdTokenKind::Identifier &&
               (equals_icase(tokenizer_.peek().text, "readonly") || equals_icase(tokenizer_.peek().text, "report")))
        {
            const std::string marker = tokenizer_.next().text;
            if (equals_icase(marker, "readonly"))
            {
                variable.read_only = true;
            }
            else
            {
                variable.reportable = true;
            }
        }

        if (is_operator(tokenizer_.peek(), ":"))
        {
            tokenizer_.next();
            if (variable.type == SourceFgdValueType::Flags)
            {
                add_error(tokenizer_.peek(), "flag sets do not have long names");
                return false;
            }

            if (!consume_value(variable.display_name, "variable display name"))
            {
                return false;
            }

            if (is_operator(tokenizer_.peek(), ":"))
            {
                tokenizer_.next();
                if (!is_operator(tokenizer_.peek(), ":"))
                {
                    if (!consume_value(variable.default_value, "variable default value"))
                    {
                        return false;
                    }

                    if (value_type_stores_integer(variable.type))
                    {
                        variable.default_integer = parse_int(variable.default_value).value_or(0);
                    }
                }
            }

            if (is_operator(tokenizer_.peek(), ":"))
            {
                tokenizer_.next();
                if (!consume_value(variable.description, "variable description"))
                {
                    return false;
                }
            }
        }
        else
        {
            variable.display_name = variable.name;
        }

        if (!finalize_boolean_variable(variable))
        {
            return false;
        }

        if (is_operator(tokenizer_.peek(), "="))
        {
            if (variable.type != SourceFgdValueType::Flags && variable.type != SourceFgdValueType::Choices)
            {
                add_error(tokenizer_.peek(), "didn't expect '=' here");
                return false;
            }

            if (!parse_choices(variable))
            {
                return false;
            }
        }
        else if (variable.type == SourceFgdValueType::Flags || variable.type == SourceFgdValueType::Choices)
        {
            if (variable.choices.empty())
            {
                add_error(tokenizer_.peek(), "no choices or flags specified");
                return false;
            }
        }

        return true;
    }

    bool parse_members(SourceFgdEntityClass& entity_class)
    {
        while (true)
        {
            const FgdToken peek = tokenizer_.peek();
            if (peek.kind == FgdTokenKind::End)
            {
                add_error(peek, "expected ']'");
                return false;
            }
            if (is_operator(peek, "]"))
            {
                return true;
            }

            if (peek.kind == FgdTokenKind::Identifier && equals_icase(peek.text, "input"))
            {
                tokenizer_.next();
                SourceFgdInputOutput input;
                if (!parse_input_output(input))
                {
                    return false;
                }
                entity_class.inputs.push_back(std::move(input));
                continue;
            }

            if (peek.kind == FgdTokenKind::Identifier && equals_icase(peek.text, "output"))
            {
                tokenizer_.next();
                SourceFgdInputOutput output;
                if (!parse_input_output(output))
                {
                    return false;
                }
                entity_class.outputs.push_back(std::move(output));
                continue;
            }

            if (peek.kind == FgdTokenKind::Identifier && equals_icase(peek.text, "key"))
            {
                tokenizer_.next();
            }

            SourceFgdVariable variable;
            if (!parse_variable(variable))
            {
                return false;
            }

            upsert_variable(entity_class, std::move(variable));
        }
    }

    void parse_class(std::string_view section)
    {
        SourceFgdEntityClass entity_class;
        entity_class.kind = class_kind_from_section(section);

        if (!parse_specifiers(entity_class) || !consume_operator("=") || !consume_identifier(entity_class.name, "class name"))
        {
            skip_until_next_section();
            return;
        }

        if (is_operator(tokenizer_.peek(), ":"))
        {
            tokenizer_.next();
            if (!consume_value(entity_class.description, "class description"))
            {
                skip_until_next_section();
                return;
            }
        }

        if (!consume_operator("[") || !parse_members(entity_class) || !consume_operator("]"))
        {
            skip_until_next_section();
            return;
        }

        game_data_.upsert_class(std::move(entity_class));
    }

    void parse_map_size()
    {
        int min_coord = 0;
        int max_coord = 0;
        if (!consume_operator("(") || !consume_int(min_coord, "minimum map coordinate") || !consume_operator(",") ||
            !consume_int(max_coord, "maximum map coordinate") || !consume_operator(")"))
        {
            skip_until_next_section();
            return;
        }

        if (min_coord != max_coord)
        {
            game_data_.min_map_coord_ = std::min(min_coord, max_coord);
            game_data_.max_map_coord_ = std::max(min_coord, max_coord);
        }
    }

    void parse_grid_nav()
    {
        SourceFgdGridNav grid_nav;
        if (!consume_operator("(") || !consume_int(grid_nav.edge_size, "gridnav edge size") || !consume_operator(",") ||
            !consume_int(grid_nav.offset_x, "gridnav offset x") || !consume_operator(",") ||
            !consume_int(grid_nav.offset_y, "gridnav offset y") || !consume_operator(",") ||
            !consume_int(grid_nav.trace_height, "gridnav trace height") || !consume_operator(")"))
        {
            skip_until_next_section();
            return;
        }

        game_data_.grid_nav_ = grid_nav;
    }

    void parse_material_exclusion()
    {
        if (!consume_operator("["))
        {
            skip_until_next_section();
            return;
        }

        while (!is_operator(tokenizer_.peek(), "]") && tokenizer_.peek().kind != FgdTokenKind::End)
        {
            std::string directory;
            if (!consume_value(directory, "material exclusion directory"))
            {
                skip_until_next_section();
                return;
            }

            const auto it = std::find_if(game_data_.material_exclusions_.begin(), game_data_.material_exclusions_.end(), [&](const auto& existing) {
                return equals_icase(existing.directory, directory);
            });
            if (it == game_data_.material_exclusions_.end())
            {
                game_data_.material_exclusions_.push_back(SourceFgdMaterialExclusion{.directory = std::move(directory), .user_generated = false});
            }
        }

        if (!consume_operator("]"))
        {
            skip_until_next_section();
        }
    }

    void parse_auto_vis_group()
    {
        SourceFgdAutoVisGroup group;
        if (!consume_operator("=") || !consume_value(group.parent, "auto visgroup parent") || !consume_operator("["))
        {
            skip_until_next_section();
            return;
        }

        while (is_value_token(tokenizer_.peek()))
        {
            SourceFgdAutoVisGroupClass group_class;
            if (!consume_value(group_class.name, "auto visgroup class") || !consume_operator("["))
            {
                skip_until_next_section();
                return;
            }

            while (is_value_token(tokenizer_.peek()))
            {
                std::string entity_name;
                if (!consume_value(entity_name, "auto visgroup entity"))
                {
                    skip_until_next_section();
                    return;
                }
                group_class.entities.push_back(std::move(entity_name));
            }

            if (!consume_operator("]"))
            {
                skip_until_next_section();
                return;
            }

            group.classes.push_back(std::move(group_class));
        }

        if (!consume_operator("]"))
        {
            skip_until_next_section();
            return;
        }

        game_data_.auto_vis_groups_.push_back(std::move(group));
    }

    SourceFgdGameData& game_data_;
    FgdTokenizer tokenizer_;
    std::filesystem::path source_path_;
};

bool SourceFgdGameData::load_file(const std::filesystem::path& path)
{
    clear();
    return load_file_internal(path) && errors_.empty();
}

bool SourceFgdGameData::load_text(std::string_view text, std::filesystem::path source_path)
{
    clear();
    SourceFgdParser parser(*this, text, std::move(source_path));
    return parser.parse() && errors_.empty();
}

bool SourceFgdGameData::load_file_internal(const std::filesystem::path& path)
{
    const std::filesystem::path normalized = normalized_absolute(path);
    if (std::find(include_stack_.begin(), include_stack_.end(), normalized) != include_stack_.end())
    {
        return true;
    }

    std::string text;
    try
    {
        text = read_text_file(normalized);
    }
    catch (const std::exception& error)
    {
        errors_.push_back(error.what());
        return false;
    }

    include_stack_.push_back(normalized);
    SourceFgdParser parser(*this, text, normalized);
    const bool result = parser.parse();
    include_stack_.pop_back();
    return result;
}

void SourceFgdGameData::clear()
{
    classes_.clear();
    material_exclusions_.clear();
    auto_vis_groups_.clear();
    errors_.clear();
    include_stack_.clear();
    min_map_coord_ = -8192;
    max_map_coord_ = 8192;
    grid_nav_.reset();
}

const SourceFgdEntityClass* SourceFgdGameData::find_class(std::string_view class_name) const
{
    const auto it = std::find_if(classes_.begin(), classes_.end(), [&](const SourceFgdEntityClass& entity_class) {
        return equals_icase(entity_class.name, class_name);
    });
    return it == classes_.end() ? nullptr : &*it;
}

const std::vector<SourceFgdEntityClass>& SourceFgdGameData::classes() const
{
    return classes_;
}

const std::vector<SourceFgdMaterialExclusion>& SourceFgdGameData::material_exclusions() const
{
    return material_exclusions_;
}

const std::vector<SourceFgdAutoVisGroup>& SourceFgdGameData::auto_vis_groups() const
{
    return auto_vis_groups_;
}

const std::vector<std::string>& SourceFgdGameData::errors() const
{
    return errors_;
}

int SourceFgdGameData::min_map_coord() const
{
    return min_map_coord_;
}

int SourceFgdGameData::max_map_coord() const
{
    return max_map_coord_;
}

std::optional<SourceFgdGridNav> SourceFgdGameData::grid_nav() const
{
    return grid_nav_;
}

void SourceFgdGameData::upsert_class(SourceFgdEntityClass entity_class)
{
    const auto it = std::find_if(classes_.begin(), classes_.end(), [&](const SourceFgdEntityClass& existing) {
        return equals_icase(existing.name, entity_class.name);
    });

    if (it == classes_.end())
    {
        classes_.push_back(std::move(entity_class));
    }
    else
    {
        *it = std::move(entity_class);
    }
}

SourceFgdValueType source_fgd_value_type_from_string(std::string_view text)
{
    for (const ValueTypeMapEntry& entry : kValueTypes)
    {
        if (equals_icase(text, entry.name))
        {
            return entry.type;
        }
    }

    return SourceFgdValueType::Bad;
}

std::string_view to_string(SourceFgdValueType type)
{
    for (const ValueTypeMapEntry& entry : kValueTypes)
    {
        if (entry.type == type)
        {
            return entry.name;
        }
    }

    return "unknown";
}

SourceFgdIoType source_fgd_io_type_from_string(std::string_view text)
{
    for (const IoTypeMapEntry& entry : kIoTypes)
    {
        if (equals_icase(text, entry.name))
        {
            return entry.type;
        }
    }

    return SourceFgdIoType::Invalid;
}

std::string_view to_string(SourceFgdIoType type)
{
    for (const IoTypeMapEntry& entry : kIoTypes)
    {
        if (entry.type == type)
        {
            return entry.name;
        }
    }

    return "unknown";
}

std::string_view to_string(SourceFgdClassKind kind)
{
    switch (kind)
    {
    case SourceFgdClassKind::Base:
        return "base";
    case SourceFgdClassKind::Point:
        return "point";
    case SourceFgdClassKind::Solid:
        return "solid";
    case SourceFgdClassKind::Move:
        return "move";
    case SourceFgdClassKind::KeyFrame:
        return "keyframe";
    case SourceFgdClassKind::Npc:
        return "npc";
    case SourceFgdClassKind::Filter:
        return "filter";
    }

    return "unknown";
}
}
