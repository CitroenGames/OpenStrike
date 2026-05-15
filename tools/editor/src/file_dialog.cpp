#include "file_dialog.hpp"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <commdlg.h>
#include <shlobj.h>
#endif

namespace FileDialog
{

std::string OpenFile(const char* filter, const char* defaultExt)
{
#ifdef _WIN32
    char filename[MAX_PATH] = {};
    OPENFILENAMEA ofn = {};
    ofn.lStructSize  = sizeof(ofn);
    ofn.hwndOwner    = nullptr;
    ofn.lpstrFilter  = filter;
    ofn.lpstrFile    = filename;
    ofn.nMaxFile     = MAX_PATH;
    ofn.lpstrDefExt  = defaultExt;
    ofn.Flags        = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
    if (GetOpenFileNameA(&ofn))
        return filename;
#endif
    return {};
}

std::string SaveFile(const char* filter, const char* defaultExt)
{
#ifdef _WIN32
    char filename[MAX_PATH] = {};
    OPENFILENAMEA ofn = {};
    ofn.lStructSize  = sizeof(ofn);
    ofn.hwndOwner    = nullptr;
    ofn.lpstrFilter  = filter;
    ofn.lpstrFile    = filename;
    ofn.nMaxFile     = MAX_PATH;
    ofn.lpstrDefExt  = defaultExt;
    ofn.Flags        = OFN_OVERWRITEPROMPT | OFN_NOCHANGEDIR;
    if (GetSaveFileNameA(&ofn))
        return filename;
#endif
    return {};
}

std::string BrowseFolder(const char* title)
{
#ifdef _WIN32
    BROWSEINFOA bi = {};
    bi.lpszTitle = title;
    bi.ulFlags   = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE;
    LPITEMIDLIST pidl = SHBrowseForFolderA(&bi);
    if (pidl)
    {
        char path[MAX_PATH] = {};
        SHGetPathFromIDListA(pidl, path);
        CoTaskMemFree(pidl);
        return path;
    }
#endif
    return {};
}

} // namespace FileDialog
