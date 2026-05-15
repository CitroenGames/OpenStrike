#include "model_preview_panel.hpp"

#include <imgui.h>

void ModelPreviewPanel::Init(ModelCache* cache, Rhi* rhi)
{
    m_modelCache = cache;
    m_rhi = rhi;
}

void ModelPreviewPanel::Shutdown()
{
}

void ModelPreviewPanel::Open(const std::string& modelPath, const std::string& vpkName)
{
    m_modelPath = modelPath;
    m_vpkName = vpkName;
    m_open = true;
}

void ModelPreviewPanel::Draw()
{
    if (!m_open)
        return;

    ImGui::Begin("Model Preview", &m_open);
    if (m_modelPath.empty())
        ImGui::TextDisabled("No model selected.");
    else
        ImGui::TextWrapped("%s", m_modelPath.c_str());
    ImGui::TextDisabled("The model viewport will be reconnected after the OpenStrike asset browser is in place.");
    ImGui::End();
}
