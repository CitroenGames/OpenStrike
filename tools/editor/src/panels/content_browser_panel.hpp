#pragma once

#include <functional>
#include <string>

class Rhi;
class CPackedStore;

constexpr const char* PAYLOAD_TYPE_MODEL = "MODEL_PATH";
constexpr const char* PAYLOAD_TYPE_MATERIAL = "MATERIAL_PATH";

struct VPKFileEntry
{
    std::string name;
    std::string extension;
    std::string fullPath;
    unsigned int size = 0;
    bool isDirectory = false;
};

class ContentBrowserPanel
{
public:
    void Init(Rhi* rhi);
    void Shutdown();
    void Draw();

    void SetGameDir(const char* dir);
    void SetLogFunc(std::function<void(const char*)> fn) { m_logFunc = std::move(fn); }
    void SetModelOpenFunc(std::function<void(const std::string&, const std::string&)> fn) { m_onModelOpen = std::move(fn); }
    void SetResOpenFunc(std::function<void(const std::string&, int)> fn) { m_onResOpen = std::move(fn); }

    void ImportFBX(const std::string& absFilePath, const std::string& gameDir);
    void ReplaceVPKFile(const std::string& vpkPath, const std::string& diskFilePath);
    void ImportFileToVPK(const std::string& diskFilePath);
    CPackedStore* GetArchiveStore(int idx);
    void NotifyVPKChanged(int archiveIdx);
    std::string GetCurrentBrowsePath() const;
    int GetSelectedArchive() const { return -1; }
    const VPKFileEntry* GetSelectedFile() const { return nullptr; }

    bool& IsOpen() { return m_open; }

private:
    void Log(const char* fmt, ...);

    bool m_open = true;
    std::string m_gameDir;
    std::function<void(const char*)> m_logFunc;
    std::function<void(const std::string&, const std::string&)> m_onModelOpen;
    std::function<void(const std::string&, int)> m_onResOpen;
};
