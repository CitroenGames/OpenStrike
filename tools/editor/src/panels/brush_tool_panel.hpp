#pragma once

#include <string>
#include <functional>

class VmfDocument;

class BrushToolPanel
{
public:
    void Init();
    void Shutdown();
    void Draw(VmfDocument* doc);

    bool& IsOpen() { return m_open; }

    bool IsCreationActive() const { return m_creationActive; }
    void SetCreationActive(bool active) { m_creationActive = active; }
    void ClearCreation() { m_creationActive = false; }

    float GetGridSize() const { return m_gridSize; }
    float GetDefaultHeight() const { return m_defaultHeight; }
    const std::string& GetDefaultMaterial() const { return m_defaultMaterial; }

    using CreateCallback = std::function<void()>;
    void SetCreateCallback(CreateCallback cb) { m_createCallback = cb; }
    void OnBrushCreated() { if (m_createCallback) m_createCallback(); }

private:
    bool m_open = true;
    bool m_creationActive = false;
    float m_gridSize = 64.0f;
    float m_defaultHeight = 64.0f;
    std::string m_defaultMaterial = "DEV/DEV_MEASUREGENERIC01";
    CreateCallback m_createCallback;
};
