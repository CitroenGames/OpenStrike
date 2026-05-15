#include "console_panel.hpp"

#include <imgui.h>
#include <cstdarg>
#include <cstdio>

void ConsolePanel::Init()
{
}

void ConsolePanel::Shutdown()
{
}

void ConsolePanel::Draw()
{
    if (!m_open)
        return;

    ImGui::Begin("Console", &m_open);

    if (ImGui::Button("Clear"))
        Clear();

    ImGui::SameLine();

    if (ImGui::Button("Copy"))
        CopyToClipboard();

    ImGui::Separator();

    ImGui::BeginChild("##ConsoleScroll", ImVec2(0, 0), false, ImGuiWindowFlags_HorizontalScrollbar);
    for (const auto& line : m_lines)
        ImGui::TextUnformatted(line.c_str());

    if (m_autoScroll && ImGui::GetScrollY() >= ImGui::GetScrollMaxY())
        ImGui::SetScrollHereY(1.0f);

    ImGui::EndChild();

    ImGui::End();
}

void ConsolePanel::AddLog(const char* fmt, ...)
{
    char buf[1024];
    va_list args;
    va_start(args, fmt);
    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    m_lines.emplace_back(buf);
}

void ConsolePanel::Clear()
{
    m_lines.clear();
}

void ConsolePanel::CopyToClipboard()
{
    if (m_lines.empty())
        return;

    std::string allText;
    for (const auto& line : m_lines)
    {
        allText += line;
        allText += '\n';
    }

    ImGui::SetClipboardText(allText.c_str());
}
