#pragma once

#include <string>

class VmfDocument;

enum class DispToolMode { Create, Sculpt, Smooth, Paint, TagWalkable, TagBuildable };
enum class DispPaintAxis { Face, X, Y, Z, Subdivision };
enum class DispBrushType { Soft, Hard };

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
    DispPaintAxis GetPaintAxis() const { return m_paintAxis; }
    DispBrushType GetBrushType() const { return m_brushType; }

private:
    bool m_open = true;
    bool m_active = false;
    DispToolMode m_mode = DispToolMode::Sculpt;
    int m_power = 3;
    float m_brushSize = 64.0f;
    float m_brushStrength = 5.0f;
    float m_brushFalloff = 1.0f;
    float m_paintAlpha = 255.0f;
    DispPaintAxis m_paintAxis = DispPaintAxis::Face;
    DispBrushType m_brushType = DispBrushType::Soft;
};
