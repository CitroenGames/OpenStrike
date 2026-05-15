#include "res_editor_panel.hpp"

#include "console_panel.hpp"

#include <cstdlib>
#include <imgui.h>

void ResEditorPanel::Init()
{
}

void ResEditorPanel::Shutdown()
{
}

void ResEditorPanel::Draw()
{
    if (!m_open)
        return;

    ImGui::Begin("RES Editor", &m_open);
    ImGui::TextWrapped("%s", m_path.empty() ? "No RES file open." : m_path.c_str());
    ImGui::TextDisabled("RES editing is deferred in the OpenStrike editor port.");
    ImGui::End();
}

void ResEditorPanel::OpenFile(const std::string& path, ConsolePanel* console)
{
    m_path = path;
    m_open = true;
    if (console)
        console->AddLog("RES editor is not wired yet: %s", path.c_str());
}

void ResEditorPanel::OpenFromVPK(const std::string&, const std::string& vpkPath, CPackedStore*, ConsolePanel* console)
{
    m_path = vpkPath;
    m_open = true;
    if (console)
        console->AddLog("VPK-backed RES editing is not wired yet: %s", vpkPath.c_str());
}

int ResEditorPanel::ParsePosValue(const std::string& val, int parentExtent, bool isSize, bool proportional, int propBase)
{
    int parsed = std::atoi(val.c_str());
    if (proportional && propBase > 0)
        parsed = parsed * parentExtent / propBase;
    return isSize && parsed < 0 ? 0 : parsed;
}
