#pragma once

#include <string>

class VmfDocument;

enum class ClipAxis { X, Y, Z };
enum class ClipMode { KeepBoth, KeepFront, KeepBack };

class ClipToolPanel
{
public:
    void Init();
    void Shutdown();
    void Draw(VmfDocument* doc);

    bool& IsOpen() { return m_open; }

    bool IsClipActive() const { return m_clipActive; }
    void SetClipActive(bool active) { m_clipActive = active; }
    void ClearClip() { m_clipActive = false; }

    ClipAxis GetAxis() const { return m_axis; }
    void SetAxis(ClipAxis a) { m_axis = a; }
    void CycleClipAxis();

    ClipMode GetMode() const { return m_mode; }
    void SetMode(ClipMode m) { m_mode = m; }
    void CycleClipMode();

    float GetGridSize() const { return m_gridSize; }
    const std::string& GetClipMaterial() const { return m_clipMaterial; }

private:
    bool m_open = true;
    bool m_clipActive = false;
    ClipAxis m_axis = ClipAxis::X;
    ClipMode m_mode = ClipMode::KeepBoth;
    float m_gridSize = 64.0f;
    std::string m_clipMaterial = "DEV/DEV_MEASUREGENERIC01";
};
