#pragma once

#include <string>
#include <vector>
#include <memory>

struct KVNode
{
    std::string key;
    std::string value;
    std::vector<std::unique_ptr<KVNode>> children;

    bool IsBlock() const { return !children.empty(); }

    const char*    GetString(const char* childKey, const char* defaultVal = "") const;
    int            GetInt(const char* childKey, int defaultVal = 0) const;
    float          GetFloat(const char* childKey, float defaultVal = 0.0f) const;
    const KVNode*  FindChild(const char* childKey) const;
    std::vector<const KVNode*> FindChildren(const char* childKey) const;
};

struct KVParseResult
{
    std::vector<std::unique_ptr<KVNode>> roots;
    std::string error;
    int         errorLine = 0;
    bool        ok = false;
};

KVParseResult KV_Parse(const char* text, size_t length);
