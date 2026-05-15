#include "displacement_tool_panel.hpp"
#include "../vmf_document.hpp"

#include <imgui.h>

void DisplacementToolPanel::Init()
{
}

void DisplacementToolPanel::Shutdown()
{
}

void DisplacementToolPanel::Draw(VmfDocument* doc)
{
    if (!m_open)
        return;

    ImGui::Begin("Displacement Tool", &m_open);

    if (!doc)
    {
        ImGui::TextDisabled("No document. Use File > New or Open.");
        ImGui::End();
        return;
    }

    // Active toggle
    if (m_active)
    {
        ImGui::TextColored(ImVec4(0.2f, 1.0f, 0.2f, 1.0f), "Displacement Tool Active");
        if (ImGui::Button("Deactivate", ImVec2(-1, 0)))
            m_active = false;
    }
    else
    {
        if (ImGui::Button("Activate Displacement Tool", ImVec2(-1, 0)))
            m_active = true;
    }

    if (!m_active)
    {
        ImGui::End();
        return;
    }

    ImGui::Separator();

    // Mode selector
    ImGui::Text("Mode:");

    auto ModeButton = [&](const char* label, DispToolMode mode) {
        bool selected = (m_mode == mode);
        if (selected)
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.3f, 0.5f, 0.8f, 1.0f));
        if (ImGui::Button(label, ImVec2(ImGui::GetContentRegionAvail().x * 0.24f, 0)))
            m_mode = mode;
        if (selected)
            ImGui::PopStyleColor();
        ImGui::SameLine();
    };

    ModeButton("Create", DispToolMode::Create);
    ModeButton("Sculpt", DispToolMode::Sculpt);
    ModeButton("Smooth", DispToolMode::Smooth);
    ModeButton("Paint", DispToolMode::Paint);
    ImGui::NewLine();

    ImGui::Separator();

    // Mode-specific settings
    if (m_mode == DispToolMode::Create)
    {
        ImGui::Text("Create Displacement");
        ImGui::TextWrapped("Click on a brush face in the viewport to create a displacement surface.");

        ImGui::Separator();
        ImGui::Text("Power:");
        ImGui::RadioButton("2 (5x5)", &m_power, 2); ImGui::SameLine();
        ImGui::RadioButton("3 (9x9)", &m_power, 3); ImGui::SameLine();
        ImGui::RadioButton("4 (17x17)", &m_power, 4);
    }
    else
    {
        // Brush settings for Sculpt, Smooth, Paint
        ImGui::Text("Brush Settings:");

        ImGui::SliderFloat("Size", &m_brushSize, 8.0f, 512.0f, "%.0f");
        ImGui::SliderFloat("Strength", &m_brushStrength, 0.01f, 1.0f, "%.2f");
        ImGui::SliderFloat("Falloff", &m_brushFalloff, 0.5f, 4.0f, "%.1f");

        if (m_mode == DispToolMode::Sculpt)
        {
            ImGui::Separator();
            ImGui::TextWrapped("LMB: Raise terrain\nShift+LMB: Lower terrain");
        }
        else if (m_mode == DispToolMode::Smooth)
        {
            ImGui::Separator();
            ImGui::TextWrapped("LMB: Smooth terrain");
        }
        else if (m_mode == DispToolMode::Paint)
        {
            ImGui::Separator();
            ImGui::SliderFloat("Target Alpha", &m_paintAlpha, 0.0f, 255.0f, "%.0f");
            ImGui::TextWrapped("LMB: Paint alpha values\nShift+LMB: Erase (paint to 0)");
        }
    }

    ImGui::End();
}
