#pragma once

#include <string>

namespace FileDialog
{
    std::string OpenFile(const char* filter, const char* defaultExt);
    std::string SaveFile(const char* filter, const char* defaultExt);
    std::string BrowseFolder(const char* title);
}
