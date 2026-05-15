#include "outliner_panel.hpp"
#include "../vmf_document.hpp"

#include <imgui.h>
#include <cstdio>
#include <string>

void OutlinerPanel::Init()
{
}

void OutlinerPanel::Shutdown()
{
}

void OutlinerPanel::Draw(VmfDocument* doc)
{
    if (!m_open)
        return;

    ImGui::Begin("Outliner", &m_open);

    if (!doc)
    {
        ImGui::TextDisabled("No map loaded.");
        ImGui::End();
        return;
    }

    const auto& world = doc->GetWorld();
    const auto& entities = doc->GetEntities();
    const auto& sel = doc->GetSelection();

    // World solids
    if (ImGui::TreeNodeEx("worldspawn", ImGuiTreeNodeFlags_DefaultOpen))
    {
        for (int i = 0; i < (int)world.solids.size(); i++)
        {
            char label[64];
            snprintf(label, sizeof(label), "Solid %d", world.solids[i].id);
            bool selected = (sel.type == SelectionType::WorldSolid && sel.index == i);
            if (ImGui::Selectable(label, selected))
                doc->SetSelection({ SelectionType::WorldSolid, i, -1 });
        }
        ImGui::TreePop();
    }

    // Entities
    for (int i = 0; i < (int)entities.size(); i++)
    {
        const auto& ent = entities[i];
        char label[128];
        const char* targetname = ent.GetValue("targetname", "");
        if (targetname[0])
            snprintf(label, sizeof(label), "%s \"%s\" (id %d)", ent.classname.c_str(), targetname, ent.id);
        else
            snprintf(label, sizeof(label), "%s (id %d)", ent.classname.c_str(), ent.id);

        ImGui::PushID(i);

        if (ent.solids.empty())
        {
            // Point entity
            bool selected = (sel.type == SelectionType::Entity && sel.index == i);
            if (ImGui::Selectable(label, selected))
                doc->SetSelection({ SelectionType::Entity, i, -1 });
        }
        else
        {
            // Brush entity with child solids
            ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_OpenOnArrow;
            if (sel.type == SelectionType::Entity && sel.index == i)
                flags |= ImGuiTreeNodeFlags_Selected;

            bool nodeOpen = ImGui::TreeNodeEx(label, flags);
            if (ImGui::IsItemClicked() && !ImGui::IsItemToggledOpen())
                doc->SetSelection({ SelectionType::Entity, i, -1 });

            if (nodeOpen)
            {
                for (int j = 0; j < (int)ent.solids.size(); j++)
                {
                    char solidLabel[64];
                    snprintf(solidLabel, sizeof(solidLabel), "Solid %d", ent.solids[j].id);
                    bool solidSel = (sel.type == SelectionType::EntitySolid
                                     && sel.index == i && sel.subIndex == j);
                    if (ImGui::Selectable(solidLabel, solidSel))
                        doc->SetSelection({ SelectionType::EntitySolid, i, j });
                }
                ImGui::TreePop();
            }
        }

        // Right-click context menu for entities
        if (ImGui::BeginPopupContextItem())
        {
            if (ImGui::MenuItem("Copy Properties"))
            {
                std::string text;
                text += "classname = " + ent.classname + "\n";
                for (const auto& kv : ent.keyvalues)
                {
                    if (kv.first == "classname")
                        continue;
                    text += kv.first + " = " + kv.second + "\n";
                }
                ImGui::SetClipboardText(text.c_str());
            }
            ImGui::EndPopup();
        }

        ImGui::PopID();
    }

    ImGui::End();
}
