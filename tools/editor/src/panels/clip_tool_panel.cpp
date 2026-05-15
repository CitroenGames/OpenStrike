#include "clip_tool_panel.hpp"
#include "../vmf_document.hpp"

#include <imgui.h>
#include <cstring>

void ClipToolPanel::Init()
{
}

void ClipToolPanel::Shutdown()
{
}

void ClipToolPanel::CycleClipAxis()
{
    switch (m_axis)
    {
    case ClipAxis::X: m_axis = ClipAxis::Y; break;
    case ClipAxis::Y: m_axis = ClipAxis::Z; break;
    case ClipAxis::Z: m_axis = ClipAxis::X; break;
    }
}

void ClipToolPanel::CycleClipMode()
{
    switch (m_mode)
    {
    case ClipMode::KeepBoth:  m_mode = ClipMode::KeepFront; break;
    case ClipMode::KeepFront: m_mode = ClipMode::KeepBack;  break;
    case ClipMode::KeepBack:  m_mode = ClipMode::KeepBoth;  break;
    }
}

void ClipToolPanel::Draw(VmfDocument* doc)
{
    if (!m_open)
        return;

    ImGui::Begin("Clip Tool", &m_open);

    if (!doc)
    {
        ImGui::TextDisabled("No document. Use File > New or Open.");
        ImGui::End();
        return;
    }

    if (m_clipActive)
    {
        ImGui::TextColored(ImVec4(1.0f, 1.0f, 0.2f, 1.0f), "Clip Tool Active");

        const char* axisNames[] = { "X", "Y", "Z" };
        ImGui::Text("Axis: %s", axisNames[(int)m_axis]);
        ImGui::SameLine();
        if (ImGui::SmallButton("Cycle Axis (C)"))
            CycleClipAxis();

        const char* modeNames[] = { "Keep Both", "Keep Front", "Keep Back" };
        ImGui::Text("Mode: %s", modeNames[(int)m_mode]);
        ImGui::SameLine();
        if (ImGui::SmallButton("Cycle Mode (Tab)"))
            CycleClipMode();

        ImGui::Separator();
        ImGui::TextWrapped("Click in the viewport to position the clip plane. "
                           "Left click to confirm, right click or Escape to cancel.");

        if (ImGui::Button("Cancel"))
            m_clipActive = false;
    }
    else
    {
        const auto& sel = doc->GetSelection();
        bool hasBrush = (sel.type == SelectionType::WorldSolid && sel.index >= 0
                         && sel.index < (int)doc->GetWorld().solids.size());

        if (!hasBrush)
            ImGui::BeginDisabled();

        if (ImGui::Button("Clip Brush", ImVec2(-1, 0)))
            m_clipActive = true;

        if (!hasBrush)
        {
            ImGui::EndDisabled();
            ImGui::TextDisabled("Select a brush first.");
        }
    }

    ImGui::Separator();

    ImGui::Text("Settings:");

    const char* gridSizes[] = { "16", "32", "64", "128", "256" };
    int currentGrid = 2;
    if (m_gridSize == 16) currentGrid = 0;
    else if (m_gridSize == 32) currentGrid = 1;
    else if (m_gridSize == 64) currentGrid = 2;
    else if (m_gridSize == 128) currentGrid = 3;
    else if (m_gridSize == 256) currentGrid = 4;

    if (ImGui::Combo("Grid Size", &currentGrid, gridSizes, 5))
    {
        const float sizes[] = { 16, 32, 64, 128, 256 };
        m_gridSize = sizes[currentGrid];
    }

    char matBuf[256];
    strncpy(matBuf, m_clipMaterial.c_str(), sizeof(matBuf) - 1);
    matBuf[sizeof(matBuf) - 1] = '\0';
    if (ImGui::InputText("Clip Material", matBuf, sizeof(matBuf)))
        m_clipMaterial = matBuf;

    ImGui::End();
}
