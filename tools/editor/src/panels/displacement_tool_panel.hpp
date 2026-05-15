#pragma once

#include <string>

class VmfDocument;

enum class DispToolMode { Create, Sculpt, Smooth, Paint };

class DisplacementToolPanel
{
public:
    void Init();
    void Shutdown();
    void Draw(VmfDocument* doc);

    bool& IsOpen() { return m_open; }

    bool IsActive() const { return m_active; }
    void SetActive(bool active) { m_active = active; }

    DispToolMode GetMode() const { return m_mode; }
    int GetPower() const { return m_power; }
    float GetBrushSize() const { return m_brushSize; }
    float GetBrushStrength() const { return m_brushStrength; }
    float GetBrushFalloff() const { return m_brushFalloff; }
    float GetPaintAlpha() const { return m_paintAlpha; }

private:
    bool m_open = true;
    bool m_active = false;
    DispToolMode m_mode = DispToolMode::Sculpt;
    int m_power = 3;
    float m_brushSize = 64.0f;
    float m_brushStrength = 0.3f;
    float m_brushFalloff = 2.0f;
    float m_paintAlpha = 255.0f;
};
