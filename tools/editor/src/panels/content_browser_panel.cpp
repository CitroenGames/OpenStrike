#include "content_browser_panel.hpp"

#include <cstdarg>
#include <cstdio>
#include <filesystem>
#include <imgui.h>

void ContentBrowserPanel::Init(Rhi*)
{
}

void ContentBrowserPanel::Shutdown()
{
}

void ContentBrowserPanel::SetGameDir(const char* dir)
{
    m_gameDir = dir ? dir : "";
    Log("Content browser game directory: %s", m_gameDir.empty() ? "(none)" : m_gameDir.c_str());
}

void ContentBrowserPanel::Draw()
{
    if (!m_open)
        return;

    ImGui::Begin("Content Browser", &m_open);
    if (m_gameDir.empty())
    {
        ImGui::TextDisabled("No game directory selected.");
    }
    else
    {
        ImGui::TextWrapped("%s", m_gameDir.c_str());
        ImGui::Separator();
        ImGui::TextDisabled("OpenStrike asset browsing is not wired to editable VPK archives yet.");
    }
    ImGui::End();
}

void ContentBrowserPanel::ImportFBX(const std::string&, const std::string&)
{
    Log("FBX import is not part of the OpenStrike editor port yet.");
}

void ContentBrowserPanel::ReplaceVPKFile(const std::string&, const std::string&)
{
    Log("VPK replacement is not part of the OpenStrike editor port yet.");
}

void ContentBrowserPanel::ImportFileToVPK(const std::string&)
{
    Log("VPK import is not part of the OpenStrike editor port yet.");
}

CPackedStore* ContentBrowserPanel::GetArchiveStore(int)
{
    return nullptr;
}

void ContentBrowserPanel::NotifyVPKChanged(int)
{
}

std::string ContentBrowserPanel::GetCurrentBrowsePath() const
{
    return m_gameDir;
}

void ContentBrowserPanel::Log(const char* fmt, ...)
{
    if (!m_logFunc)
        return;

    char buffer[512];
    va_list args;
    va_start(args, fmt);
    vsnprintf(buffer, sizeof(buffer), fmt, args);
    va_end(args);
    m_logFunc(buffer);
}
