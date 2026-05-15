#include "editor_theme.hpp"
#include <imgui.h>

void ApplyUnrealTheme()
{
    ImGuiStyle& style = ImGui::GetStyle();
    ImVec4* colors = style.Colors;

    // -- Style variables (flat, clean, minimal rounding) --
    style.WindowPadding     = ImVec2(8.0f, 8.0f);
    style.WindowRounding    = 2.0f;
    style.WindowBorderSize  = 1.0f;
    style.ChildRounding     = 0.0f;
    style.ChildBorderSize   = 1.0f;
    style.PopupRounding     = 2.0f;
    style.PopupBorderSize   = 1.0f;
    style.FramePadding      = ImVec2(5.0f, 4.0f);
    style.FrameRounding     = 2.0f;
    style.FrameBorderSize   = 0.0f;
    style.ItemSpacing       = ImVec2(8.0f, 4.0f);
    style.ItemInnerSpacing  = ImVec2(4.0f, 4.0f);
    style.IndentSpacing     = 16.0f;
    style.ScrollbarSize     = 14.0f;
    style.ScrollbarRounding = 2.0f;
    style.GrabMinSize       = 10.0f;
    style.GrabRounding      = 2.0f;
    style.TabRounding       = 2.0f;
    style.TabBorderSize     = 0.0f;
    style.TabBarBorderSize  = 1.0f;
    style.TabBarOverlineSize = 2.0f;
    style.DockingSeparatorSize = 2.0f;
    style.WindowTitleAlign  = ImVec2(0.0f, 0.5f);
    style.SeparatorTextBorderSize = 1.0f;

    // -- Colors --
    // Text
    colors[ImGuiCol_Text]                   = ImVec4(0.878f, 0.878f, 0.878f, 1.00f);
    colors[ImGuiCol_TextDisabled]           = ImVec4(0.439f, 0.439f, 0.439f, 1.00f);
    colors[ImGuiCol_TextLink]               = ImVec4(0.102f, 0.541f, 0.831f, 1.00f);
    colors[ImGuiCol_TextSelectedBg]         = ImVec4(0.055f, 0.431f, 0.722f, 0.40f);

    // Backgrounds
    colors[ImGuiCol_WindowBg]               = ImVec4(0.141f, 0.141f, 0.141f, 1.00f);
    colors[ImGuiCol_ChildBg]                = ImVec4(0.118f, 0.118f, 0.118f, 1.00f);
    colors[ImGuiCol_PopupBg]                = ImVec4(0.118f, 0.118f, 0.118f, 0.98f);
    colors[ImGuiCol_MenuBarBg]              = ImVec4(0.102f, 0.102f, 0.102f, 1.00f);

    // Borders
    colors[ImGuiCol_Border]                 = ImVec4(0.227f, 0.227f, 0.227f, 1.00f);
    colors[ImGuiCol_BorderShadow]           = ImVec4(0.000f, 0.000f, 0.000f, 0.00f);

    // Frame (input fields, checkboxes, sliders)
    colors[ImGuiCol_FrameBg]                = ImVec4(0.102f, 0.102f, 0.102f, 1.00f);
    colors[ImGuiCol_FrameBgHovered]         = ImVec4(0.200f, 0.200f, 0.200f, 1.00f);
    colors[ImGuiCol_FrameBgActive]          = ImVec4(0.055f, 0.431f, 0.722f, 1.00f);

    // Title bar
    colors[ImGuiCol_TitleBg]                = ImVec4(0.102f, 0.102f, 0.102f, 1.00f);
    colors[ImGuiCol_TitleBgActive]          = ImVec4(0.165f, 0.165f, 0.165f, 1.00f);
    colors[ImGuiCol_TitleBgCollapsed]       = ImVec4(0.102f, 0.102f, 0.102f, 0.75f);

    // Scrollbar
    colors[ImGuiCol_ScrollbarBg]            = ImVec4(0.102f, 0.102f, 0.102f, 0.60f);
    colors[ImGuiCol_ScrollbarGrab]          = ImVec4(0.259f, 0.259f, 0.259f, 1.00f);
    colors[ImGuiCol_ScrollbarGrabHovered]   = ImVec4(0.314f, 0.314f, 0.314f, 1.00f);
    colors[ImGuiCol_ScrollbarGrabActive]    = ImVec4(0.376f, 0.376f, 0.376f, 1.00f);

    // Checkmark, slider
    colors[ImGuiCol_CheckMark]              = ImVec4(0.102f, 0.541f, 0.831f, 1.00f);
    colors[ImGuiCol_SliderGrab]             = ImVec4(0.055f, 0.431f, 0.722f, 1.00f);
    colors[ImGuiCol_SliderGrabActive]       = ImVec4(0.102f, 0.541f, 0.831f, 1.00f);

    // Buttons
    colors[ImGuiCol_Button]                 = ImVec4(0.200f, 0.200f, 0.200f, 1.00f);
    colors[ImGuiCol_ButtonHovered]          = ImVec4(0.275f, 0.275f, 0.275f, 1.00f);
    colors[ImGuiCol_ButtonActive]           = ImVec4(0.055f, 0.431f, 0.722f, 1.00f);

    // Headers (collapsing headers, tree nodes, selectables, menu items)
    colors[ImGuiCol_Header]                 = ImVec4(0.165f, 0.165f, 0.165f, 1.00f);
    colors[ImGuiCol_HeaderHovered]          = ImVec4(0.227f, 0.227f, 0.227f, 1.00f);
    colors[ImGuiCol_HeaderActive]           = ImVec4(0.055f, 0.431f, 0.722f, 1.00f);

    // Separator
    colors[ImGuiCol_Separator]              = ImVec4(0.227f, 0.227f, 0.227f, 1.00f);
    colors[ImGuiCol_SeparatorHovered]       = ImVec4(0.055f, 0.431f, 0.722f, 0.78f);
    colors[ImGuiCol_SeparatorActive]        = ImVec4(0.055f, 0.431f, 0.722f, 1.00f);

    // Resize grip
    colors[ImGuiCol_ResizeGrip]             = ImVec4(0.259f, 0.259f, 0.259f, 0.25f);
    colors[ImGuiCol_ResizeGripHovered]      = ImVec4(0.055f, 0.431f, 0.722f, 0.67f);
    colors[ImGuiCol_ResizeGripActive]       = ImVec4(0.055f, 0.431f, 0.722f, 0.95f);

    // Input text cursor
    colors[ImGuiCol_InputTextCursor]        = ImVec4(0.878f, 0.878f, 0.878f, 1.00f);

    // Tabs
    colors[ImGuiCol_Tab]                    = ImVec4(0.102f, 0.102f, 0.102f, 1.00f);
    colors[ImGuiCol_TabHovered]             = ImVec4(0.200f, 0.200f, 0.200f, 1.00f);
    colors[ImGuiCol_TabSelected]            = ImVec4(0.141f, 0.141f, 0.141f, 1.00f);
    colors[ImGuiCol_TabSelectedOverline]    = ImVec4(0.055f, 0.431f, 0.722f, 1.00f);
    colors[ImGuiCol_TabDimmed]              = ImVec4(0.082f, 0.082f, 0.082f, 1.00f);
    colors[ImGuiCol_TabDimmedSelected]      = ImVec4(0.122f, 0.122f, 0.122f, 1.00f);
    colors[ImGuiCol_TabDimmedSelectedOverline] = ImVec4(0.376f, 0.376f, 0.376f, 1.00f);

    // Docking
    colors[ImGuiCol_DockingPreview]         = ImVec4(0.055f, 0.431f, 0.722f, 0.70f);
    colors[ImGuiCol_DockingEmptyBg]         = ImVec4(0.102f, 0.102f, 0.102f, 1.00f);

    // Plot
    colors[ImGuiCol_PlotLines]              = ImVec4(0.529f, 0.529f, 0.529f, 1.00f);
    colors[ImGuiCol_PlotLinesHovered]       = ImVec4(0.878f, 0.565f, 0.125f, 1.00f);
    colors[ImGuiCol_PlotHistogram]          = ImVec4(0.878f, 0.565f, 0.125f, 1.00f);
    colors[ImGuiCol_PlotHistogramHovered]   = ImVec4(1.000f, 0.600f, 0.000f, 1.00f);

    // Tables
    colors[ImGuiCol_TableHeaderBg]          = ImVec4(0.165f, 0.165f, 0.165f, 1.00f);
    colors[ImGuiCol_TableBorderStrong]      = ImVec4(0.227f, 0.227f, 0.227f, 1.00f);
    colors[ImGuiCol_TableBorderLight]       = ImVec4(0.173f, 0.173f, 0.173f, 1.00f);
    colors[ImGuiCol_TableRowBg]             = ImVec4(0.000f, 0.000f, 0.000f, 0.00f);
    colors[ImGuiCol_TableRowBgAlt]          = ImVec4(1.000f, 1.000f, 1.000f, 0.03f);

    // Tree lines
    colors[ImGuiCol_TreeLines]              = ImVec4(0.227f, 0.227f, 0.227f, 1.00f);

    // Drag and drop
    colors[ImGuiCol_DragDropTarget]         = ImVec4(0.878f, 0.565f, 0.125f, 0.90f);
    colors[ImGuiCol_DragDropTargetBg]       = ImVec4(0.000f, 0.000f, 0.000f, 0.00f);

    // Misc
    colors[ImGuiCol_UnsavedMarker]          = ImVec4(0.878f, 0.565f, 0.125f, 1.00f);
    colors[ImGuiCol_NavCursor]              = ImVec4(0.055f, 0.431f, 0.722f, 1.00f);
    colors[ImGuiCol_NavWindowingHighlight]  = ImVec4(1.000f, 1.000f, 1.000f, 0.70f);
    colors[ImGuiCol_NavWindowingDimBg]      = ImVec4(0.000f, 0.000f, 0.000f, 0.20f);
    colors[ImGuiCol_ModalWindowDimBg]       = ImVec4(0.000f, 0.000f, 0.000f, 0.55f);
}
