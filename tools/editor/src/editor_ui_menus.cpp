#include "editor_ui.hpp"

#include "editor_profiler.hpp"
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
#include "panels/viewport_panel.hpp"
#include "vmf_document.hpp"

#include <SDL3/SDL.h>
#include <imgui.h>

namespace
{
void QueueQuitEvent()
{
    SDL_Event quit;
    quit.type = SDL_EVENT_QUIT;
    SDL_PushEvent(&quit);
}
}

void EditorUI::DrawMenuBar()
{
    if (!ImGui::BeginMenuBar())
        return;

    DrawFileMenu();
    DrawEditMenu();
    DrawMapMenu();
    DrawViewMenu();
    DrawToolsMenu();
    DrawHelpMenu();

    ImGui::EndMenuBar();
}

void EditorUI::DrawFileMenu()
{
    if (!ImGui::BeginMenu("File"))
        return;

    if (ImGui::MenuItem("New", "Ctrl+N"))
        NewFile();
    if (ImGui::MenuItem("Open...", "Ctrl+O"))
        OpenFile();
    if (ImGui::MenuItem("Add FGD..."))
        OpenFGD();
    if (ImGui::MenuItem("Clear FGDs", nullptr, false, m_fgd->IsLoaded()))
    {
        m_fgd->ClearFGD();
        m_console->AddLog("FGDs cleared.");
        if (m_document)
            m_viewport->SetDocument(m_document.get(), m_fgd.get());
    }

    ImGui::Separator();
    if (ImGui::MenuItem("Set Game Directory..."))
        BrowseGameDir();

    ImGui::Separator();
    if (ImGui::MenuItem("Save", "Ctrl+S", false, m_document != nullptr))
        Save();
    if (ImGui::MenuItem("Save As...", "Ctrl+Shift+S", false, m_document != nullptr))
        SaveAs();
    if (ImGui::MenuItem("Export as .MAP...", nullptr, false, m_document != nullptr))
        ExportMap();

    ImGui::Separator();
    if (ImGui::MenuItem("Run Map...", "F9", false, m_document != nullptr))
        RunMap();

    ImGui::Separator();
    if (ImGui::MenuItem("Exit"))
        QueueQuitEvent();

    ImGui::EndMenu();
}

void EditorUI::DrawEditMenu()
{
    if (!ImGui::BeginMenu("Edit"))
        return;

    if (ImGui::MenuItem("Undo", "Ctrl+Z")) {}
    if (ImGui::MenuItem("Redo", "Ctrl+Y")) {}

    ImGui::Separator();
    if (ImGui::MenuItem("Delete Selected", "Delete", false,
        m_document && m_document->GetSelection().type == SelectionType::Entity))
    {
        int idx = m_document->GetSelection().index;
        m_document->RemoveEntity(idx);
        NotifySceneChanged();
    }

    ImGui::Separator();
    if (ImGui::MenuItem("Make Hollow...", "Ctrl+H", false,
        m_document && m_document->GetSelection().type == SelectionType::WorldSolid))
        m_showHollowDialog = true;

    ImGui::EndMenu();
}

void EditorUI::DrawMapMenu()
{
    if (!ImGui::BeginMenu("Map"))
        return;

    if (ImGui::MenuItem("Load Pointfile...", nullptr, false, m_document != nullptr))
        LoadPointfile();
    if (ImGui::MenuItem("Clear Pointfile", nullptr, false, m_hasLeakTrail))
        ClearPointfile();

    ImGui::Separator();

    if (m_document)
    {
        auto& kv = m_document->GetWorldMut().keyvalues;
        bool allowLeaks = (kv.count("allow_leaks") && kv["allow_leaks"] == "1");
        if (ImGui::Checkbox("Allow Leaks", &allowLeaks))
        {
            kv["allow_leaks"] = allowLeaks ? "1" : "0";
            m_document->MarkDirty();
        }
        if (ImGui::IsItemHovered())
        {
            ImGui::SetTooltip("Auto-seal the map with a skybox during compile.\n"
                              "Allows maps with leaks to compile and run.");
        }
    }

    ImGui::EndMenu();
}

void EditorUI::DrawViewMenu()
{
    if (!ImGui::BeginMenu("View"))
        return;

    ImGui::MenuItem("3D Viewport", nullptr, &m_viewport->IsOpen());
    ImGui::MenuItem("Console",     nullptr, &m_console->IsOpen());
    ImGui::MenuItem("Properties",  nullptr, &m_properties->IsOpen());
    ImGui::MenuItem("Outliner",    nullptr, &m_outliner->IsOpen());
    ImGui::MenuItem("Content Browser", nullptr, &m_contentBrowser->IsOpen());
    ImGui::MenuItem("Entity Tool", nullptr, &m_entityTool->IsOpen());
    ImGui::MenuItem("Brush Tool",  nullptr, &m_brushTool->IsOpen());
    ImGui::MenuItem("Clip Tool",   nullptr, &m_clipTool->IsOpen());
    ImGui::MenuItem("Displacement Tool", nullptr, &m_dispTool->IsOpen());
    ImGui::MenuItem("Model Preview", nullptr, &m_modelPreview->IsOpen());

    ImGui::Separator();
    if (ImGui::BeginMenu("Tool Textures"))
    {
        auto& vis = m_viewport->GetRenderer().m_toolVis;
        bool changed = false;
        if (ImGui::Checkbox("Skip",        &vis.showSkip))       changed = true;
        if (ImGui::Checkbox("Hint",        &vis.showHint))        changed = true;
        if (ImGui::Checkbox("Nodraw",      &vis.showNodraw))      changed = true;
        if (ImGui::Checkbox("Player Clip", &vis.showPlayerClip))  changed = true;
        if (ImGui::Checkbox("Area Portal", &vis.showAreaPortal))  changed = true;
        if (ImGui::Checkbox("Ladder",      &vis.showLadder))      changed = true;
        if (ImGui::Checkbox("Trigger",     &vis.showTrigger))     changed = true;
        if (changed)
            m_viewport->MarkDirty();
        ImGui::EndMenu();
    }

    ImGui::Separator();
    auto& renderer = m_viewport->GetRenderer();
    ImGui::Checkbox("BSP Wireframe",          &renderer.m_showBspWireframe);
    ImGui::Checkbox("Displacement Wireframe", &renderer.m_showDispWireframe);

    ImGui::EndMenu();
}

void EditorUI::DrawToolsMenu()
{
    if (!ImGui::BeginMenu("Tools"))
        return;

    auto& profiler = EditorProfiler::Instance();
    if (profiler.IsActive())
    {
        ImGui::TextDisabled("Profiler active (%zu events)", profiler.GetEventCount());
        if (ImGui::MenuItem("Stop Profiler"))
            profiler.EndSession();
    }
    else
    {
        if (ImGui::MenuItem("Start VPK Profiler"))
        {
            profiler.BeginSession("VPK_Manual");
            profiler.SetThreadName("Main Thread");
        }
    }

    bool hasEvents = profiler.GetEventCount() > 0;
    if (ImGui::MenuItem("Save Trace to File...", nullptr, false, hasEvents))
    {
        std::string tracePath = profiler.FlushToFile();
        if (!tracePath.empty())
            m_console->AddLog("Trace saved: %s", tracePath.c_str());
        else
            m_console->AddLog("Failed to save trace file.");
    }

    ImGui::EndMenu();
}

void EditorUI::DrawHelpMenu()
{
    if (!ImGui::BeginMenu("Help"))
        return;

    if (ImGui::MenuItem("About"))
        m_showAbout = true;

    ImGui::EndMenu();
}
