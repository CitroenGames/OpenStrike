#include "properties_panel.hpp"
#include "../vmf_document.hpp"
#include "../fgd_manager.hpp"

#include <imgui.h>
#include <cstdio>
#include <cstring>

void PropertiesPanel::Init()
{
}

void PropertiesPanel::Shutdown()
{
}

void PropertiesPanel::Draw(VmfDocument* doc, FgdManager* fgd)
{
    if (!m_open)
        return;

    ImGui::Begin("Properties", &m_open);

    if (!doc || doc->GetSelection().type == SelectionType::None)
    {
        ImGui::TextDisabled("No selection.");
        ImGui::End();
        return;
    }

    const auto& sel = doc->GetSelection();
    const char* classname = doc->GetSelectedClassname();

    if (classname && classname[0])
        ImGui::Text("Class: %s", classname);

    // Show editable origin for entities
    VmfEntity* ent = doc->GetSelectedEntity();
    if (ent)
    {
        ImGui::SeparatorText("Transform");
        Vec3 origin = ent->GetOrigin();
        float pos[3] = { origin.x, origin.y, origin.z };
        if (ImGui::DragFloat3("Origin", pos, 1.0f, -65536.0f, 65536.0f, "%.0f"))
        {
            Vec3 newOrigin = { pos[0], pos[1], pos[2] };
            doc->MoveEntity(sel.index, newOrigin);
            if (m_sceneChanged) m_sceneChanged();
        }
    }

    // Show key-values for entity selections
    auto* kv = doc->GetSelectedKeyValuesMut();
    if (kv && !kv->empty())
    {
        ImGui::SeparatorText("Key-Values");
        if (ImGui::BeginTable("##KV", 2,
            ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_Resizable))
        {
            ImGui::TableSetupColumn("Key", ImGuiTableColumnFlags_WidthStretch, 0.4f);
            ImGui::TableSetupColumn("Value", ImGuiTableColumnFlags_WidthStretch, 0.6f);
            ImGui::TableHeadersRow();

            int idx = 0;
            for (auto& [k, v] : *kv)
            {
                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0);
                ImGui::TextUnformatted(k.c_str());
                ImGui::TableSetColumnIndex(1);

                ImGui::PushID(idx++);
                char buf[256];
                strncpy(buf, v.c_str(), sizeof(buf) - 1);
                buf[sizeof(buf) - 1] = '\0';
                ImGui::SetNextItemWidth(-1);
                if (ImGui::InputText("##val", buf, sizeof(buf), ImGuiInputTextFlags_EnterReturnsTrue))
                {
                    v = buf;
                    doc->MarkDirty();
                    if (k == "origin" || k == "classname" || k == "model")
                    {
                        if (m_sceneChanged) m_sceneChanged();
                    }
                }
                ImGui::PopID();
            }
            ImGui::EndTable();
        }

        // Add new key-value
        ImGui::Spacing();
        ImGui::Text("Add Key-Value:");
        ImGui::SetNextItemWidth(120);
        ImGui::InputText("##newkey", m_editKey, sizeof(m_editKey));
        ImGui::SameLine();
        ImGui::SetNextItemWidth(180);
        ImGui::InputText("##newval", m_editVal, sizeof(m_editVal));
        ImGui::SameLine();
        if (ImGui::Button("Add") && m_editKey[0])
        {
            (*kv)[m_editKey] = m_editVal;
            doc->MarkDirty();
            m_editKey[0] = '\0';
            m_editVal[0] = '\0';
            if (m_sceneChanged) m_sceneChanged();
        }
    }

    // Show solid info for solid selections
    if (sel.type == SelectionType::WorldSolid)
    {
        int idx = sel.index;
        const auto& solids = doc->GetWorld().solids;
        if (idx >= 0 && idx < (int)solids.size())
        {
            const auto& solid = solids[idx];
            ImGui::SeparatorText("Solid");
            ImGui::Text("ID: %d", solid.id);
            ImGui::Text("Sides: %d", (int)solid.sides.size());

            if (ImGui::TreeNode("Materials"))
            {
                for (const auto& side : solid.sides)
                    ImGui::BulletText("%s", side.material.c_str());
                ImGui::TreePop();
            }

            // Tie to Entity conversion
            ImGui::SeparatorText("Convert Brush");
            static int selectedBrushClass = 0;
            const char* brushClasses[] = {
                "func_detail", "func_brush", "func_illusionary",
                "trigger_once", "trigger_multiple", "func_door"
            };
            ImGui::Combo("Entity Class", &selectedBrushClass, brushClasses, IM_ARRAYSIZE(brushClasses));
            if (ImGui::Button("Tie to Entity", ImVec2(-1, 0)))
            {
                doc->TieWorldSolidToEntity(idx, brushClasses[selectedBrushClass]);
                if (m_sceneChanged) m_sceneChanged();
            }
        }
    }
    else if (sel.type == SelectionType::EntitySolid)
    {
        int entIdx = sel.index;
        int solidIdx = sel.subIndex;
        const auto& entities = doc->GetEntities();
        if (entIdx >= 0 && entIdx < (int)entities.size()
            && solidIdx >= 0 && solidIdx < (int)entities[entIdx].solids.size())
        {
            const auto& solid = entities[entIdx].solids[solidIdx];
            ImGui::SeparatorText("Solid");
            ImGui::Text("ID: %d", solid.id);
            ImGui::Text("Sides: %d", (int)solid.sides.size());

            if (ImGui::TreeNode("Materials"))
            {
                for (const auto& side : solid.sides)
                    ImGui::BulletText("%s", side.material.c_str());
                ImGui::TreePop();
            }

            // Move to World conversion
            ImGui::SeparatorText("Convert Brush");
            if (ImGui::Button("Move to World", ImVec2(-1, 0)))
            {
                doc->MoveEntitySolidsToWorld(entIdx);
                if (m_sceneChanged) m_sceneChanged();
            }
            ImGui::TextDisabled("Moves all solids from this entity to worldspawn.");
        }
    }

    // Also show Move to World for Entity selection if it has solids
    if (sel.type == SelectionType::Entity && ent && !ent->solids.empty())
    {
        ImGui::SeparatorText("Convert Brush");
        if (ImGui::Button("Move to World", ImVec2(-1, 0)))
        {
            doc->MoveEntitySolidsToWorld(sel.index);
            if (m_sceneChanged) m_sceneChanged();
        }
        ImGui::TextDisabled("Moves all %d solid(s) to worldspawn.", (int)ent->solids.size());
    }

    ImGui::End();
}
