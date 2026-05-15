#pragma once

#include <string>

class ModelCache;
class Rhi;

class ModelPreviewPanel
{
public:
    void Init(ModelCache* cache, Rhi* rhi);
    void Shutdown();
    void Draw();
    void Open(const std::string& modelPath, const std::string& vpkName = "");

    bool& IsOpen() { return m_open; }

private:
    ModelCache* m_modelCache = nullptr;
    Rhi* m_rhi = nullptr;
    std::string m_modelPath;
    std::string m_vpkName;
    bool m_open = false;
};
