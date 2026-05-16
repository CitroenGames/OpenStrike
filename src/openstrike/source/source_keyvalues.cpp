#include "openstrike/source/source_keyvalues.hpp"

#include "openstrike/source/source_paths.hpp"

#include <cstdlib>

namespace openstrike
{
namespace
{
enum class TokenType
{
    String,
    OpenBrace,
    CloseBrace,
    EndOfFile
};

struct Token
{
    TokenType type = TokenType::EndOfFile;
    std::string text;
};

struct Tokenizer
{
    const char* cur = nullptr;
    const char* end = nullptr;
    int line = 1;

    explicit Tokenizer(std::string_view text)
        : cur(text.data())
        , end(text.data() + text.size())
    {
    }

    void skip_whitespace_and_comments()
    {
        while (cur < end)
        {
            if (*cur == '\n')
            {
                ++cur;
                ++line;
            }
            else if (*cur == '\r')
            {
                ++cur;
                if (cur < end && *cur == '\n')
                {
                    ++cur;
                }
                ++line;
            }
            else if (*cur == ' ' || *cur == '\t')
            {
                ++cur;
            }
            else if (cur + 1 < end && cur[0] == '/' && cur[1] == '/')
            {
                cur += 2;
                while (cur < end && *cur != '\n' && *cur != '\r')
                {
                    ++cur;
                }
            }
            else
            {
                break;
            }
        }
    }

    Token next()
    {
        skip_whitespace_and_comments();

        if (cur >= end)
        {
            return {TokenType::EndOfFile, {}};
        }

        if (*cur == '{')
        {
            ++cur;
            return {TokenType::OpenBrace, "{"};
        }
        if (*cur == '}')
        {
            ++cur;
            return {TokenType::CloseBrace, "}"};
        }

        if (*cur == '"')
        {
            ++cur;
            std::string text;
            while (cur < end && *cur != '"')
            {
                if (*cur == '\\' && cur + 1 < end)
                {
                    ++cur;
                    switch (*cur)
                    {
                    case '"':
                        text += '"';
                        break;
                    case '\\':
                        text += '\\';
                        break;
                    case 'n':
                        text += '\n';
                        break;
                    case 't':
                        text += '\t';
                        break;
                    default:
                        text += '\\';
                        text += *cur;
                        break;
                    }
                    ++cur;
                }
                else
                {
                    if (*cur == '\n')
                    {
                        ++line;
                    }
                    text += *cur++;
                }
            }
            if (cur < end)
            {
                ++cur;
            }
            return {TokenType::String, std::move(text)};
        }

        if (*cur == '[')
        {
            std::string text;
            while (cur < end)
            {
                const char ch = *cur++;
                text += ch;
                if (ch == ']')
                {
                    break;
                }
                if (ch == '\n')
                {
                    ++line;
                }
            }
            return {TokenType::String, std::move(text)};
        }

        std::string text;
        while (cur < end && *cur != ' ' && *cur != '\t' && *cur != '\n' && *cur != '\r' && *cur != '{' &&
               *cur != '}' && *cur != '"')
        {
            text += *cur++;
        }
        return {TokenType::String, std::move(text)};
    }

    Token peek()
    {
        const char* saved_cur = cur;
        const int saved_line = line;
        Token token = next();
        cur = saved_cur;
        line = saved_line;
        return token;
    }
};

bool parse_block(Tokenizer& tokenizer, SourceKeyValueNode& parent, std::string& error)
{
    while (true)
    {
        const Token peeked = tokenizer.peek();
        if (peeked.type == TokenType::CloseBrace || peeked.type == TokenType::EndOfFile)
        {
            break;
        }

        Token key = tokenizer.next();
        if (key.type != TokenType::String)
        {
            error = "Expected key string at line " + std::to_string(tokenizer.line);
            return false;
        }

        const Token next = tokenizer.peek();
        if (next.type == TokenType::OpenBrace)
        {
            tokenizer.next();
            auto node = std::make_unique<SourceKeyValueNode>();
            node->key = std::move(key.text);
            if (!parse_block(tokenizer, *node, error))
            {
                return false;
            }

            const Token close_brace = tokenizer.next();
            if (close_brace.type != TokenType::CloseBrace)
            {
                error = "Expected '}' at line " + std::to_string(tokenizer.line);
                return false;
            }
            parent.children.push_back(std::move(node));
        }
        else if (next.type == TokenType::String)
        {
            Token value = tokenizer.next();
            auto node = std::make_unique<SourceKeyValueNode>();
            node->key = std::move(key.text);
            node->value = std::move(value.text);
            parent.children.push_back(std::move(node));
        }
        else
        {
            error = "Expected value or '{' after key at line " + std::to_string(tokenizer.line);
            return false;
        }
    }

    return true;
}

bool key_equals_ci(std::string_view lhs, std::string_view rhs)
{
    return source_lower_copy(lhs) == source_lower_copy(rhs);
}
}

const char* SourceKeyValueNode::GetString(const char* child_key, const char* default_value) const
{
    for (const auto& child : children)
    {
        if (child->key == child_key && !child->is_block())
        {
            return child->value.c_str();
        }
    }
    return default_value;
}

int SourceKeyValueNode::GetInt(const char* child_key, int default_value) const
{
    const char* child_value = GetString(child_key, nullptr);
    return child_value != nullptr ? std::atoi(child_value) : default_value;
}

float SourceKeyValueNode::GetFloat(const char* child_key, float default_value) const
{
    const char* child_value = GetString(child_key, nullptr);
    return child_value != nullptr ? static_cast<float>(std::atof(child_value)) : default_value;
}

const SourceKeyValueNode* SourceKeyValueNode::FindChild(const char* child_key) const
{
    for (const auto& child : children)
    {
        if (child->key == child_key && child->is_block())
        {
            return child.get();
        }
    }
    return nullptr;
}

std::vector<const SourceKeyValueNode*> SourceKeyValueNode::FindChildren(const char* child_key) const
{
    std::vector<const SourceKeyValueNode*> result;
    for (const auto& child : children)
    {
        if (child->key == child_key && child->is_block())
        {
            result.push_back(child.get());
        }
    }
    return result;
}

SourceKeyValueParseResult parse_source_keyvalues(std::string_view text)
{
    SourceKeyValueParseResult result;
    Tokenizer tokenizer(text);
    SourceKeyValueNode virtual_root;
    if (!parse_block(tokenizer, virtual_root, result.error))
    {
        result.errorLine = tokenizer.line;
        return result;
    }

    result.roots = std::move(virtual_root.children);
    result.ok = true;
    return result;
}

const SourceKeyValueNode* source_kv_find_child_ci(const SourceKeyValueNode& node, std::string_view child_key)
{
    for (const auto& child : node.children)
    {
        if (child->is_block() && key_equals_ci(child->key, child_key))
        {
            return child.get();
        }
    }
    return nullptr;
}

std::optional<std::string_view> source_kv_find_value_ci(const SourceKeyValueNode& node, std::string_view child_key)
{
    for (const auto& child : node.children)
    {
        if (!child->is_block() && key_equals_ci(child->key, child_key))
        {
            return std::string_view(child->value);
        }
    }
    return std::nullopt;
}
}
