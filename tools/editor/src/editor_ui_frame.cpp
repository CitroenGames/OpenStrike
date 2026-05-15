#include "editor_ui.hpp"

#include "fgd_manager.hpp"
#include "panels/brush_tool_panel.hpp"
#include "panels/clip_tool_panel.hpp"
#include "panels/console_panel.hpp"
#include "panels/content_browser_panel.hpp"
#include "panels/displacement_tool_panel.hpp"
#include "panels/entity_tool_panel.hpp"
#include "panels/model_preview_panel.hpp"
#include "panels/outliner_panel.hpp"
#include "panels/properties_panel.hpp"
#include "panels/res_editor_panel.hpp"
#include "panels/viewport_panel.hpp"
#include "run_map_dialog.hpp"
#include "vmf_document.hpp"

#include <imgui.h>

void EditorUI::Draw()
{
    DrawTitleBar();

    if (m_showStartupDialog)
    {
        DrawStartupDialog();
        return;
    }

    DrawDockspace();
    DrawEditorPanels();
    DrawModalDialogs();
    DrawRunMapDialog();
    HandleKeyboardShortcuts();
}

void EditorUI::DrawEditorPanels()
{
    m_viewport->Draw();
    m_console->Draw();
    m_outliner->Draw(m_document.get());
    m_properties->Draw(m_document.get(), m_fgd.get());
    m_contentBrowser->Draw();
    m_entityTool->Draw(m_document.get(), m_fgd.get());
    m_brushTool->Draw(m_document.get());
    m_clipTool->Draw(m_document.get());
    m_dispTool->Draw(m_document.get());
    m_modelPreview->Draw();
    m_resEditor->Draw();
}

void EditorUI::DrawStartupDialog()
{
    ImVec2 center = ImGui::GetMainViewport()->GetCenter();
    ImGui::SetNextWindowPos(center, ImGuiCond_Always, ImVec2(0.5f, 0.5f));

    ImGuiWindowFlags flags =
        ImGuiWindowFlags_NoTitleBar |
        ImGuiWindowFlags_NoResize |
        ImGuiWindowFlags_NoMove |
        ImGuiWindowFlags_NoCollapse |
        ImGuiWindowFlags_AlwaysAutoResize;

    ImGui::Begin("##StartupDialog", nullptr, flags);

    bool loading = m_loadingGameDir.load(std::memory_order_acquire);

    ImGui::Text("Hammer 2.0");
    ImGui::Separator();

    if (loading)
    {
        ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.2f, 1.0f), "Loading game files...");
        ImGui::Spacing();
    }
    else
    {
        ImGui::Text("What would you like to do?");
    }
    ImGui::Spacing();

    float buttonWidth = 200.0f;

    ImGui::BeginDisabled(loading);
    if (ImGui::Button("New Level", ImVec2(buttonWidth, 0)))
    {
        NewFile();
        m_showStartupDialog = false;
    }

    if (ImGui::Button("Open Existing...", ImVec2(buttonWidth, 0)))
    {
        OpenFile();
        m_showStartupDialog = false;
    }
    ImGui::EndDisabled();

    ImGui::End();
}

void EditorUI::DrawModalDialogs()
{
    if (m_showAbout)
    {
        ImGui::OpenPopup("About OpenStrike");
        m_showAbout = false;
    }
    if (ImGui::BeginPopupModal("About OpenStrike", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
    {
        ImGui::Text("OpenStrike Editor");
        ImGui::Separator();
        ImGui::Text("A Source Engine level editor.");
        ImGui::Text("Built with ImGui, SDL2, and OpenGL.");
        ImGui::Spacing();
        if (ImGui::Button("Close", ImVec2(120, 0)))
            ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }

    if (m_showHollowDialog)
    {
        ImGui::OpenPopup("Make Hollow");
        m_showHollowDialog = false;
    }
    if (ImGui::BeginPopupModal("Make Hollow", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
    {
        ImGui::Text("Wall thickness (units):");
        ImGui::SetNextItemWidth(150.0f);
        ImGui::InputFloat("##thickness", &m_hollowThickness, 1.0f, 8.0f, "%.0f");
        if (m_hollowThickness < 1.0f)
            m_hollowThickness = 1.0f;

        ImGui::Spacing();
        if (ImGui::Button("OK", ImVec2(80, 0)))
        {
            if (m_document && m_document->GetSelection().type == SelectionType::WorldSolid)
            {
                int idx = m_document->GetSelection().index;
                m_document->HollowWorldSolid(idx, m_hollowThickness);
                NotifySceneChanged();
            }
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel", ImVec2(80, 0)))
            ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
}

void EditorUI::DrawRunMapDialog()
{
    m_runMapDialog->Draw(m_console.get());
    m_runMapDialog->Update(m_console.get());
}

void EditorUI::HandleKeyboardShortcuts()
{
    ImGuiIO& io = ImGui::GetIO();
    if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_N))
        NewFile();
    if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_O))
        OpenFile();
    if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_S))
    {
        if (io.KeyShift)
            SaveAs();
        else
            Save();
    }
    if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_H))
    {
        if (m_document && m_document->GetSelection().type == SelectionType::WorldSolid)
            m_showHollowDialog = true;
    }
    if (ImGui::IsKeyPressed(ImGuiKey_F9))
        RunMap();
}
