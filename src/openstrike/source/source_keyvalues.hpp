#pragma once

#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace openstrike
{
struct SourceKeyValueNode
{
    std::string key;
    std::string value;
    std::vector<std::unique_ptr<SourceKeyValueNode>> children;

    [[nodiscard]] bool is_block() const { return !children.empty(); }
    [[nodiscard]] bool IsBlock() const { return is_block(); }

    [[nodiscard]] const char* GetString(const char* child_key, const char* default_value = "") const;
    [[nodiscard]] int GetInt(const char* child_key, int default_value = 0) const;
    [[nodiscard]] float GetFloat(const char* child_key, float default_value = 0.0F) const;
    [[nodiscard]] const SourceKeyValueNode* FindChild(const char* child_key) const;
    [[nodiscard]] std::vector<const SourceKeyValueNode*> FindChildren(const char* child_key) const;
};

struct SourceKeyValueParseResult
{
    std::vector<std::unique_ptr<SourceKeyValueNode>> roots;
    std::string error;
    int errorLine = 0;
    bool ok = false;
};

[[nodiscard]] SourceKeyValueParseResult parse_source_keyvalues(std::string_view text);
[[nodiscard]] const SourceKeyValueNode* source_kv_find_child_ci(const SourceKeyValueNode& node, std::string_view child_key);
[[nodiscard]] std::optional<std::string_view> source_kv_find_value_ci(const SourceKeyValueNode& node, std::string_view child_key);
}
