#pragma once

#include <string>
#include <vector>
#include <functional>

class FgdManager;
class VmfDocument;

class EntityToolPanel
{
public:
    void Init();
    void Shutdown();
    void Draw(VmfDocument* doc, FgdManager* fgd);

    bool& IsOpen() { return m_open; }

    // Returns the classname to place, empty if none selected
    const std::string& GetSelectedClass() const { return m_selectedClass; }
    bool IsPlacementActive() const { return m_placementActive; }
    void ClearPlacement() { m_placementActive = false; }
    void SetPlacementActive(bool active) { m_placementActive = active; }

    // Callback when an entity is placed
    using PlaceCallback = std::function<void()>;
    void SetPlaceCallback(PlaceCallback cb) { m_placeCallback = cb; }
    void OnEntityPlaced() { if (m_placeCallback) m_placeCallback(); }

private:
    bool m_open = true;
    std::string m_selectedClass;
    bool m_placementActive = false;
    char m_filterBuf[128] = {};
    PlaceCallback m_placeCallback;
};
