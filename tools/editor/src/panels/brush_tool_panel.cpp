#include "brush_tool_panel.hpp"
#include "../vmf_document.hpp"

#include <imgui.h>
#include <cstring>

void BrushToolPanel::Init()
{
}

void BrushToolPanel::Shutdown()
{
}

void BrushToolPanel::Draw(VmfDocument* doc)
{
    if (!m_open)
        return;

    ImGui::Begin("Brush Tool", &m_open);

    if (!doc)
    {
        ImGui::TextDisabled("No document. Use File > New or Open.");
        ImGui::End();
        return;
    }

    if (m_creationActive)
    {
        ImGui::TextColored(ImVec4(0.2f, 1.0f, 0.2f, 1.0f), "Creating Box Brush");
        ImGui::TextWrapped("Click and drag in the 3D viewport to define the brush.");
        if (ImGui::Button("Cancel"))
            m_creationActive = false;
    }
    else
    {
        if (ImGui::Button("Create Box Brush", ImVec2(-1, 0)))
            m_creationActive = true;
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

    ImGui::DragFloat("Default Height", &m_defaultHeight, 8.0f, 16.0f, 1024.0f);

    char matBuf[256];
    strncpy(matBuf, m_defaultMaterial.c_str(), sizeof(matBuf) - 1);
    matBuf[sizeof(matBuf) - 1] = '\0';
    if (ImGui::InputText("Material", matBuf, sizeof(matBuf)))
        m_defaultMaterial = matBuf;

    ImGui::End();
}
