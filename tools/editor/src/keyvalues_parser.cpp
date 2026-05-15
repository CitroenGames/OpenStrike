#include "keyvalues_parser.hpp"
#include <cstring>
#include <cstdlib>

// KVNode accessors

const char* KVNode::GetString(const char* childKey, const char* defaultVal) const
{
    for (auto& c : children)
    {
        if (c->key == childKey && !c->IsBlock())
            return c->value.c_str();
    }
    return defaultVal;
}

int KVNode::GetInt(const char* childKey, int defaultVal) const
{
    const char* s = GetString(childKey, nullptr);
    return s ? atoi(s) : defaultVal;
}

float KVNode::GetFloat(const char* childKey, float defaultVal) const
{
    const char* s = GetString(childKey, nullptr);
    return s ? (float)atof(s) : defaultVal;
}

const KVNode* KVNode::FindChild(const char* childKey) const
{
    for (auto& c : children)
    {
        if (c->key == childKey && c->IsBlock())
            return c.get();
    }
    return nullptr;
}

std::vector<const KVNode*> KVNode::FindChildren(const char* childKey) const
{
    std::vector<const KVNode*> result;
    for (auto& c : children)
    {
        if (c->key == childKey && c->IsBlock())
            result.push_back(c.get());
    }
    return result;
}

// Tokenizer

enum class TokenType { String, OpenBrace, CloseBrace, EndOfFile };

struct Token
{
    TokenType   type;
    std::string text;
};

struct Tokenizer
{
    const char* cur;
    const char* end;
    int         line = 1;

    Tokenizer(const char* text, size_t length)
        : cur(text), end(text + length) {}

    void SkipWhitespaceAndComments()
    {
        while (cur < end)
        {
            if (*cur == '\n') { ++cur; ++line; }
            else if (*cur == '\r') { ++cur; if (cur < end && *cur == '\n') ++cur; ++line; }
            else if (*cur == ' ' || *cur == '\t') { ++cur; }
            else if (cur + 1 < end && cur[0] == '/' && cur[1] == '/')
            {
                cur += 2;
                while (cur < end && *cur != '\n' && *cur != '\r')
                    ++cur;
            }
            else break;
        }
    }

    Token Next()
    {
        SkipWhitespaceAndComments();

        if (cur >= end)
            return { TokenType::EndOfFile, {} };

        if (*cur == '{') { ++cur; return { TokenType::OpenBrace, "{" }; }
        if (*cur == '}') { ++cur; return { TokenType::CloseBrace, "}" }; }

        if (*cur == '"')
        {
            ++cur;
            std::string s;
            while (cur < end && *cur != '"')
            {
                if (*cur == '\\' && cur + 1 < end)
                {
                    ++cur;
                    switch (*cur)
                    {
                        case '"':  s += '"';  break;
                        case '\\': s += '\\'; break;
                        case 'n':  s += '\n'; break;
                        case 't':  s += '\t'; break;
                        default:   s += '\\'; s += *cur; break;
                    }
                    ++cur;
                }
                else
                {
                    if (*cur == '\n') ++line;
                    s += *cur++;
                }
            }
            if (cur < end) ++cur; // skip closing quote
            return { TokenType::String, std::move(s) };
        }

        // Unquoted token
        std::string s;
        while (cur < end && *cur != ' ' && *cur != '\t' && *cur != '\n' && *cur != '\r'
               && *cur != '{' && *cur != '}' && *cur != '"')
        {
            s += *cur++;
        }
        return { TokenType::String, std::move(s) };
    }

    Token Peek()
    {
        const char* savedCur = cur;
        int savedLine = line;
        Token t = Next();
        cur = savedCur;
        line = savedLine;
        return t;
    }
};

// Parser

static bool ParseBlock(Tokenizer& tk, KVNode& parent, std::string& error)
{
    while (true)
    {
        Token t = tk.Peek();
        if (t.type == TokenType::CloseBrace || t.type == TokenType::EndOfFile)
            break;

        Token key = tk.Next();
        if (key.type != TokenType::String)
        {
            error = "Expected key string at line " + std::to_string(tk.line);
            return false;
        }

        Token next = tk.Peek();
        if (next.type == TokenType::OpenBrace)
        {
            tk.Next(); // consume '{'
            auto node = std::make_unique<KVNode>();
            node->key = std::move(key.text);
            if (!ParseBlock(tk, *node, error))
                return false;
            Token closeBrace = tk.Next();
            if (closeBrace.type != TokenType::CloseBrace)
            {
                error = "Expected '}' at line " + std::to_string(tk.line);
                return false;
            }
            parent.children.push_back(std::move(node));
        }
        else if (next.type == TokenType::String)
        {
            Token val = tk.Next();
            auto node = std::make_unique<KVNode>();
            node->key = std::move(key.text);
            node->value = std::move(val.text);
            parent.children.push_back(std::move(node));
        }
        else
        {
            error = "Expected value or '{' after key at line " + std::to_string(tk.line);
            return false;
        }
    }
    return true;
}

KVParseResult KV_Parse(const char* text, size_t length)
{
    KVParseResult result;
    Tokenizer tk(text, length);

    // VMF has multiple root-level blocks: versioninfo{}, world{}, entity{}, ...
    // Parse them as children of a virtual root
    KVNode virtualRoot;
    if (!ParseBlock(tk, virtualRoot, result.error))
    {
        result.errorLine = tk.line;
        return result;
    }

    result.roots = std::move(virtualRoot.children);
    result.ok = true;
    return result;
}
