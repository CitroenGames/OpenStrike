#pragma once

#include <functional>

class VmfDocument;
class FgdManager;

class PropertiesPanel
{
public:
    void Init();
    void Shutdown();
    void Draw(VmfDocument* doc, FgdManager* fgd = nullptr);

    bool& IsOpen() { return m_open; }

    using SceneChangedFunc = std::function<void()>;
    void SetSceneChangedFunc(SceneChangedFunc fn) { m_sceneChanged = fn; }

private:
    bool m_open = true;
    char m_editKey[128] = {};
    char m_editVal[256] = {};
    SceneChangedFunc m_sceneChanged;
};
