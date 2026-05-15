#include "editor_ui.hpp"

#include "application.hpp"
#include "vmf_document.hpp"

#include <SDL3/SDL.h>
#include <imgui.h>
#include <imgui_internal.h>

namespace
{
constexpr float kTitleBarHeight = 30.0f;
constexpr float kTitleButtonWidth = 46.0f;
const ImU32 kTitleIconColor = IM_COL32(224, 224, 224, 255);

void QueueQuitEvent()
{
    SDL_Event quit;
    quit.type = SDL_EVENT_QUIT;
    SDL_PushEvent(&quit);
}

void PushTitleButtonStyle(const ImVec4& hovered, const ImVec4& active)
{
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0, 0, 0, 0));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, hovered);
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, active);
}

void DrawMinimizeIcon()
{
    ImVec2 btnMin = ImGui::GetItemRectMin();
    ImVec2 btnMax = ImGui::GetItemRectMax();
    float cx = (btnMin.x + btnMax.x) * 0.5f;
    float cy = (btnMin.y + btnMax.y) * 0.5f;
    ImGui::GetWindowDrawList()->AddLine(
        ImVec2(cx - 5.0f, cy), ImVec2(cx + 5.0f, cy),
        kTitleIconColor, 1.0f);
}

void DrawMaximizeIcon(bool isMaximized)
{
    ImVec2 btnMin = ImGui::GetItemRectMin();
    ImVec2 btnMax = ImGui::GetItemRectMax();
    float cx = (btnMin.x + btnMax.x) * 0.5f;
    float cy = (btnMin.y + btnMax.y) * 0.5f;

    if (isMaximized)
    {
        ImGui::GetWindowDrawList()->AddRect(
            ImVec2(cx - 3.0f, cy - 1.0f), ImVec2(cx + 5.0f, cy + 5.0f),
            kTitleIconColor, 0.0f, 0, 1.0f);
        ImGui::GetWindowDrawList()->AddRectFilled(
            ImVec2(cx - 5.0f, cy - 3.0f), ImVec2(cx + 3.0f, cy - 1.0f),
            IM_COL32(82, 82, 82, 255));
        ImGui::GetWindowDrawList()->AddRect(
            ImVec2(cx - 5.0f, cy - 3.0f), ImVec2(cx + 3.0f, cy + 3.0f),
            kTitleIconColor, 0.0f, 0, 1.0f);
    }
    else
    {
        ImGui::GetWindowDrawList()->AddRect(
            ImVec2(cx - 5.0f, cy - 4.0f), ImVec2(cx + 5.0f, cy + 4.0f),
            kTitleIconColor, 0.0f, 0, 1.0f);
    }
}

void DrawCloseIcon()
{
    ImVec2 btnMin = ImGui::GetItemRectMin();
    ImVec2 btnMax = ImGui::GetItemRectMax();
    float cx = (btnMin.x + btnMax.x) * 0.5f;
    float cy = (btnMin.y + btnMax.y) * 0.5f;
    ImGui::GetWindowDrawList()->AddLine(
        ImVec2(cx - 5.0f, cy - 5.0f), ImVec2(cx + 5.0f, cy + 5.0f),
        kTitleIconColor, 1.0f);
    ImGui::GetWindowDrawList()->AddLine(
        ImVec2(cx + 5.0f, cy - 5.0f), ImVec2(cx - 5.0f, cy + 5.0f),
        kTitleIconColor, 1.0f);
}

void UpdateTitleBarButtonRects(float buttonsStartX)
{
    auto& rects = Application::s_titleBarRects;
    rects.titleBarHeight = kTitleBarHeight;

    rects.minBtnX = buttonsStartX;
    rects.minBtnY = 0.0f;
    rects.minBtnW = kTitleButtonWidth;
    rects.minBtnH = kTitleBarHeight;

    rects.maxBtnX = buttonsStartX + kTitleButtonWidth;
    rects.maxBtnY = 0.0f;
    rects.maxBtnW = kTitleButtonWidth;
    rects.maxBtnH = kTitleBarHeight;

    rects.closeBtnX = buttonsStartX + kTitleButtonWidth * 2.0f;
    rects.closeBtnY = 0.0f;
    rects.closeBtnW = kTitleButtonWidth;
    rects.closeBtnH = kTitleBarHeight;
}

void DrawDocumentTitle(const VmfDocument* document)
{
    float textY = (kTitleBarHeight - ImGui::GetFontSize()) * 0.5f;
    ImGui::SetCursorPosY(textY);

    if (!document)
    {
        ImGui::Text("Hammer 2.0");
        return;
    }

    const std::string& filePath = document->GetFilePath();
    const char* dirtyMarker = document->IsDirty() ? "*" : "";
    if (filePath.empty())
    {
        ImGui::Text("Hammer 2.0 - [Untitled]%s", dirtyMarker);
        return;
    }

    size_t slash = filePath.find_last_of("/\\");
    const char* fileName = (slash != std::string::npos) ? filePath.c_str() + slash + 1 : filePath.c_str();
    ImGui::Text("Hammer 2.0 - %s%s", fileName, dirtyMarker);
}
}

void EditorUI::DrawTitleBar()
{
    ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImVec2 titleBarPos = viewport->Pos;
    ImVec2 titleBarSize = ImVec2(viewport->Size.x, kTitleBarHeight);

    ImGui::SetNextWindowPos(titleBarPos);
    ImGui::SetNextWindowSize(titleBarSize);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(8.0f, 0.0f));
    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.082f, 0.082f, 0.082f, 1.0f));

    ImGuiWindowFlags titleFlags =
        ImGuiWindowFlags_NoDocking |
        ImGuiWindowFlags_NoTitleBar |
        ImGuiWindowFlags_NoCollapse |
        ImGuiWindowFlags_NoResize |
        ImGuiWindowFlags_NoMove |
        ImGuiWindowFlags_NoScrollbar |
        ImGuiWindowFlags_NoScrollWithMouse |
        ImGuiWindowFlags_NoBringToFrontOnFocus |
        ImGuiWindowFlags_NoNavFocus;

    ImGui::Begin("##TitleBar", nullptr, titleFlags);
    ImGui::PopStyleColor(1);
    ImGui::PopStyleVar(3);

    DrawDocumentTitle(m_document.get());

    float buttonsStartX = titleBarSize.x - kTitleButtonWidth * 3.0f;
    ImVec2 buttonSize(kTitleButtonWidth, kTitleBarHeight);
    UpdateTitleBarButtonRects(buttonsStartX);

    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0.0f, 0.0f));

    ImGui::SetCursorPos(ImVec2(buttonsStartX, 0.0f));
    PushTitleButtonStyle(ImVec4(1, 1, 1, 0.1f), ImVec4(1, 1, 1, 0.15f));
    if (ImGui::Button("##min", buttonSize))
        SDL_MinimizeWindow(m_window);
    DrawMinimizeIcon();
    ImGui::PopStyleColor(3);

    ImGui::SetCursorPos(ImVec2(buttonsStartX + kTitleButtonWidth, 0.0f));
    PushTitleButtonStyle(ImVec4(1, 1, 1, 0.1f), ImVec4(1, 1, 1, 0.15f));
    bool isMaximized = (SDL_GetWindowFlags(m_window) & SDL_WINDOW_MAXIMIZED) != 0;
    if (ImGui::Button("##max", buttonSize))
    {
        if (isMaximized)
            SDL_RestoreWindow(m_window);
        else
            SDL_MaximizeWindow(m_window);
    }
    DrawMaximizeIcon(isMaximized);
    ImGui::PopStyleColor(3);

    ImGui::SetCursorPos(ImVec2(buttonsStartX + kTitleButtonWidth * 2.0f, 0.0f));
    PushTitleButtonStyle(ImVec4(0.906f, 0.067f, 0.137f, 1.0f), ImVec4(0.784f, 0.059f, 0.118f, 1.0f));
    if (ImGui::Button("##close", buttonSize))
        QueueQuitEvent();
    DrawCloseIcon();
    ImGui::PopStyleColor(3);

    ImGui::PopStyleVar(3);
    ImGui::End();
}

void EditorUI::DrawDockspace()
{
    ImGuiViewport* viewport = ImGui::GetMainViewport();

    float titleBarHeight = Application::s_titleBarRects.titleBarHeight;
    ImVec2 dockPos(viewport->Pos.x, viewport->Pos.y + titleBarHeight);
    ImVec2 dockSize(viewport->Size.x, viewport->Size.y - titleBarHeight);

    ImGui::SetNextWindowPos(dockPos);
    ImGui::SetNextWindowSize(dockSize);
    ImGui::SetNextWindowViewport(viewport->ID);

    ImGuiWindowFlags flags =
        ImGuiWindowFlags_NoDocking |
        ImGuiWindowFlags_NoTitleBar |
        ImGuiWindowFlags_NoCollapse |
        ImGuiWindowFlags_NoResize |
        ImGuiWindowFlags_NoMove |
        ImGuiWindowFlags_NoBringToFrontOnFocus |
        ImGuiWindowFlags_NoNavFocus |
        ImGuiWindowFlags_MenuBar;

    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
    ImGui::Begin("##DockspaceRoot", nullptr, flags);
    ImGui::PopStyleVar(3);

    ImGuiID dockspaceId = ImGui::GetID("EditorDockspace");

    if (m_firstFrame)
    {
        m_firstFrame = false;
        SetupDefaultLayout(dockspaceId);
    }

    ImGui::DockSpace(dockspaceId, ImVec2(0.0f, 0.0f), ImGuiDockNodeFlags_None);

    DrawMenuBar();

    ImGui::End();
}

void EditorUI::SetupDefaultLayout(unsigned int dockspaceId)
{
    ImGui::DockBuilderRemoveNode(dockspaceId);
    ImGui::DockBuilderAddNode(dockspaceId, ImGuiDockNodeFlags_DockSpace);

    ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImGui::DockBuilderSetNodeSize(dockspaceId, viewport->WorkSize);

    ImGuiID dockLeft, dockCenter;
    ImGui::DockBuilderSplitNode(dockspaceId, ImGuiDir_Left, 0.2f, &dockLeft, &dockCenter);

    ImGuiID dockRight, dockMiddle;
    ImGui::DockBuilderSplitNode(dockCenter, ImGuiDir_Right, 0.25f, &dockRight, &dockMiddle);

    ImGuiID dockBottom, dockViewport;
    ImGui::DockBuilderSplitNode(dockMiddle, ImGuiDir_Down, 0.25f, &dockBottom, &dockViewport);

    ImGui::DockBuilderDockWindow("3D Viewport", dockViewport);
    ImGui::DockBuilderDockWindow("Outliner",    dockLeft);
    ImGui::DockBuilderDockWindow("Entity Tool", dockLeft);
    ImGui::DockBuilderDockWindow("Brush Tool",  dockLeft);
    ImGui::DockBuilderDockWindow("Clip Tool",   dockLeft);
    ImGui::DockBuilderDockWindow("Displacement Tool", dockLeft);
    ImGui::DockBuilderDockWindow("Properties",  dockRight);
    ImGui::DockBuilderDockWindow("Console",     dockBottom);
    ImGui::DockBuilderDockWindow("Content Browser", dockBottom);

    ImGui::DockBuilderFinish(dockspaceId);
}
