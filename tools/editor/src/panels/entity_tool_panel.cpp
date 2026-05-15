#include "entity_tool_panel.hpp"
#include "../vmf_document.hpp"
#include "../fgd_manager.hpp"

#include <imgui.h>
#include <cstring>
#include <algorithm>
#include <vector>

void EntityToolPanel::Init()
{
}

void EntityToolPanel::Shutdown()
{
}

static bool CaseInsensitiveContains(const char* haystack, const char* needle)
{
    if (!needle[0]) return true;
    size_t hLen = strlen(haystack);
    size_t nLen = strlen(needle);
    if (nLen > hLen) return false;
    for (size_t i = 0; i <= hLen - nLen; i++)
    {
        bool match = true;
        for (size_t j = 0; j < nLen; j++)
        {
            if (tolower((unsigned char)haystack[i + j]) != tolower((unsigned char)needle[j]))
            {
                match = false;
                break;
            }
        }
        if (match) return true;
    }
    return false;
}

void EntityToolPanel::Draw(VmfDocument* doc, FgdManager* fgd)
{
    if (!m_open)
        return;

    ImGui::Begin("Entity Tool", &m_open);

    if (!fgd || !fgd->IsLoaded())
    {
        ImGui::TextDisabled("No FGD loaded. Use File > Add FGD...");
        ImGui::End();
        return;
    }

    if (!doc)
    {
        ImGui::TextDisabled("No document. Use File > New or Open.");
        ImGui::End();
        return;
    }

    // Placement status
    if (m_placementActive)
    {
        ImGui::TextColored(ImVec4(0.2f, 1.0f, 0.2f, 1.0f), "Placing: %s", m_selectedClass.c_str());
        ImGui::TextWrapped("Left-click in the 3D viewport to place the entity.");
        if (ImGui::Button("Cancel Placement"))
            m_placementActive = false;
        ImGui::Separator();
    }

    // Filter
    ImGui::Text("Filter:");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(-1);
    ImGui::InputText("##EntityFilter", m_filterBuf, sizeof(m_filterBuf));

    ImGui::Separator();

    // Entity class list
    const openstrike::SourceFgdGameData* gd = fgd->GetGameData();
    const std::vector<openstrike::SourceFgdEntityClass>& classes = gd->classes();

    // Collect and sort visible classes
    struct ClassEntry { std::string name; bool isPoint; };
    std::vector<ClassEntry> entries;
    for (const openstrike::SourceFgdEntityClass& cls : classes)
    {
        if (cls.kind == openstrike::SourceFgdClassKind::Base)
            continue;

        if (m_filterBuf[0] && !CaseInsensitiveContains(cls.name.c_str(), m_filterBuf))
            continue;

        const bool isPoint =
            cls.kind == openstrike::SourceFgdClassKind::Point ||
            cls.kind == openstrike::SourceFgdClassKind::Npc ||
            cls.kind == openstrike::SourceFgdClassKind::Filter ||
            cls.kind == openstrike::SourceFgdClassKind::KeyFrame;
        entries.push_back({ cls.name, isPoint });
    }

    std::sort(entries.begin(), entries.end(),
        [](const ClassEntry& a, const ClassEntry& b) {
            return a.name < b.name;
        });

    // Point entities section
    if (ImGui::CollapsingHeader("Point Entities", ImGuiTreeNodeFlags_DefaultOpen))
    {
        for (const auto& entry : entries)
        {
            if (!entry.isPoint) continue;

            bool selected = (m_selectedClass == entry.name);
            if (ImGui::Selectable(entry.name.c_str(), selected))
            {
                m_selectedClass = entry.name;
                m_placementActive = true;
            }
        }
    }

    // Solid entities section
    if (ImGui::CollapsingHeader("Brush Entities"))
    {
        for (const auto& entry : entries)
        {
            if (entry.isPoint) continue;

            bool selected = (m_selectedClass == entry.name);
            if (ImGui::Selectable(entry.name.c_str(), selected))
            {
                m_selectedClass = entry.name;
                // Brush entities can't be trivially placed with a click
                // but we select them for reference
                m_placementActive = false;
            }
        }
    }

    ImGui::End();
}
