#pragma once

#include "openstrike/source/source_keyvalues.hpp"

#include <cstddef>
#include <string_view>

using KVNode = openstrike::SourceKeyValueNode;
using KVParseResult = openstrike::SourceKeyValueParseResult;

inline KVParseResult KV_Parse(const char* text, size_t length)
{
    return openstrike::parse_source_keyvalues(std::string_view(text, length));
}
