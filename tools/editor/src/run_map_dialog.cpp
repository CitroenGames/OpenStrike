#include "run_map_dialog.hpp"
#include "panels/console_panel.hpp"
#include "file_dialog.hpp"

#include <imgui.h>
#include <filesystem>
#include <utility>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#endif

namespace fs = std::filesystem;

namespace
{
void AddCandidatePath(std::vector<fs::path>& paths, fs::path path)
{
    path = path.lexically_normal();
    for (const fs::path& existing : paths)
    {
        if (existing == path)
            return;
    }
    paths.push_back(std::move(path));
}

std::vector<fs::path> BuildToolSearchDirs(const std::string& gameDir)
{
    std::vector<fs::path> paths;
    const fs::path root = fs::path(gameDir);

    AddCandidatePath(paths, root / "bin");
    AddCandidatePath(paths, root);

    // Legacy Source SDK layout: Hammer/editor in sibling bin, game files in csgo/.
    if (!root.parent_path().empty())
        AddCandidatePath(paths, root.parent_path() / "bin");

    return paths;
}

std::vector<fs::path> BuildGameExeSearchDirs(const std::string& gameDir)
{
    std::vector<fs::path> paths;
    const fs::path root = fs::path(gameDir);

    AddCandidatePath(paths, root);
    AddCandidatePath(paths, root / "bin");

    // Legacy Source SDK layout: game executable next to the game folder.
    if (!root.parent_path().empty())
    {
        AddCandidatePath(paths, root.parent_path());
        AddCandidatePath(paths, root.parent_path() / "bin");
    }

    return paths;
}
}

RunMapDialog::RunMapDialog() = default;
RunMapDialog::~RunMapDialog() = default;

void RunMapDialog::Init()
{
    m_config.LoadFromFile(GetConfigFilePath());
}

void RunMapDialog::Open()
{
    m_wantOpen = true;
}

bool RunMapDialog::IsCompiling() const
{
    return m_process && m_process->IsRunning();
}

void RunMapDialog::SetGameDir(const std::string& gameDir)
{
    m_gameDir = gameDir;

    const std::vector<fs::path> toolDirs = BuildToolSearchDirs(gameDir);
    const std::vector<fs::path> gameExeDirs = BuildGameExeSearchDirs(gameDir);
    std::error_code ec;

    auto trySet = [&](std::string& target, const char* name) {
        if (target.empty())
        {
            for (const fs::path& dir : toolDirs)
            {
                fs::path p = dir / name;
                if (fs::is_regular_file(p, ec))
                {
                    target = p.string();
                    break;
                }
            }
        }
    };

    trySet(m_config.vbspPath, "vbsp.exe");
    trySet(m_config.vvisPath, "vvis.exe");
    trySet(m_config.vradPath, "vrad.exe");

    // Try common game executables
    if (m_config.gameExePath.empty())
    {
        for (const char* name : {"csgo.exe", "hl2.exe", "left4dead2.exe", "portal2.exe"})
        {
            for (const fs::path& dir : gameExeDirs)
            {
                fs::path p = dir / name;
                if (fs::is_regular_file(p, ec))
                {
                    m_config.gameExePath = p.string();
                    break;
                }
            }
            if (!m_config.gameExePath.empty())
                break;
        }
    }
}

// ---------------------------------------------------------------------------
// Pump compile output to console each frame
// ---------------------------------------------------------------------------
void RunMapDialog::Update(ConsolePanel* console)
{
    if (!m_process)
        return;

    auto lines = m_process->DrainLogLines();
    for (const auto& line : lines)
    {
        console->AddLog("%s", line.c_str());

        // Detect leak in compiler output
        if (!m_leakDetected && line.find("leaked") != std::string::npos)
        {
            m_leakDetected = true;
            console->AddLog(">>> Leak detected! Use Map > Load Pointfile to visualize.");
        }
    }
}

// ---------------------------------------------------------------------------
// Main dialog draw
// ---------------------------------------------------------------------------
void RunMapDialog::Draw(ConsolePanel* console)
{
    if (m_wantOpen)
    {
        ImGui::OpenPopup("Run Map");
        m_wantOpen = false;
    }

    ImVec2 center = ImGui::GetMainViewport()->GetCenter();
    ImGui::SetNextWindowPos(center, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(520, 0), ImGuiCond_Appearing);

    if (ImGui::BeginPopupModal("Run Map", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
    {
        DrawToolPathsSection();
        ImGui::Separator();
        DrawCompileOptionsSection();
        if (m_config.radMode != CompileMode::None)
            DrawAdvancedRadSection();
        ImGui::Separator();
        DrawGameOptionsSection();

        if (m_process && m_process->IsRunning())
        {
            ImGui::Separator();
            DrawProgressBar();
        }

        ImGui::Spacing();
        DrawButtons(console);

        ImGui::EndPopup();
    }
}

// ---------------------------------------------------------------------------
void RunMapDialog::DrawToolPathsSection()
{
    if (ImGui::CollapsingHeader("Tool Paths"))
    {
        float browseWidth = 30.0f;
        float inputWidth  = ImGui::GetContentRegionAvail().x - browseWidth - 70.0f;

        auto DrawPathRow = [&](const char* label, std::string& path, const char* id) {
            ImGui::Text("%s", label);
            ImGui::SameLine(70.0f);

            char buf[512];
            strncpy(buf, path.c_str(), sizeof(buf));
            buf[sizeof(buf) - 1] = '\0';

            ImGui::SetNextItemWidth(inputWidth);
            char inputId[64];
            snprintf(inputId, sizeof(inputId), "##%s", id);
            if (ImGui::InputText(inputId, buf, sizeof(buf)))
                path = buf;

            ImGui::SameLine();
            char btnId[64];
            snprintf(btnId, sizeof(btnId), "...##%s_br", id);
            if (ImGui::Button(btnId))
                BrowseForExe(path);
        };

        DrawPathRow("VBSP:", m_config.vbspPath, "vbsp");
        DrawPathRow("VVIS:", m_config.vvisPath, "vvis");
        DrawPathRow("VRAD:", m_config.vradPath, "vrad");
        DrawPathRow("Game:", m_config.gameExePath, "game");
    }
}

// ---------------------------------------------------------------------------
void RunMapDialog::DrawCompileOptionsSection()
{
    ImGui::Text("BSP Compile:");
    int bsp = (int)m_config.bspMode;
    ImGui::RadioButton("No##bsp", &bsp, 0); ImGui::SameLine();
    ImGui::RadioButton("Normal##bsp", &bsp, 1); ImGui::SameLine();
    ImGui::RadioButton("Only Entities##bsp", &bsp, 2);
    m_config.bspMode = (BspMode)bsp;

    ImGui::Spacing();

    ImGui::Text("VIS Compile:");
    int vis = (int)m_config.visMode;
    ImGui::RadioButton("No##vis", &vis, 0); ImGui::SameLine();
    ImGui::RadioButton("Normal##vis", &vis, 1); ImGui::SameLine();
    ImGui::RadioButton("Fast##vis", &vis, 2);
    m_config.visMode = (CompileMode)vis;

    ImGui::Spacing();

    ImGui::Text("RAD Compile:");
    int rad = (int)m_config.radMode;
    ImGui::RadioButton("No##rad", &rad, 0); ImGui::SameLine();
    ImGui::RadioButton("Fast##rad", &rad, 2); ImGui::SameLine();
    ImGui::RadioButton("Normal##rad", &rad, 1); ImGui::SameLine();
    ImGui::RadioButton("Final##rad", &rad, 3);
    m_config.radMode = (CompileMode)rad;

    ImGui::Checkbox("HDR lighting", &m_config.hdrLight);
}

// ---------------------------------------------------------------------------
void RunMapDialog::DrawAdvancedRadSection()
{
    if (!ImGui::CollapsingHeader("Advanced Lighting Options"))
        return;

    ImGui::Indent(10.0f);

    ImGui::Text("Ambient Occlusion");
    ImGui::SliderFloat("AO Radius##rad", &m_config.aoRadius, 1.0f, 128.0f, "%.0f");
    ImGui::SliderInt("AO Samples##rad", &m_config.aoSamples, 8, 128);

    ImGui::Spacing();
    ImGui::Text("Sampling");
    ImGui::SliderInt("Sun Samples##rad", &m_config.sunSamples, 64, 2048);
    ImGui::SliderFloat("Normal Bias##rad", &m_config.sampleNormalBias, 0.05f, 2.0f, "%.2f");
    ImGui::SliderFloat("Static Prop Shadow Bias##rad", &m_config.staticPropFudge, 0.25f, 8.0f, "%.1f");

    ImGui::Spacing();
    ImGui::Checkbox("Accurate form factor", &m_config.accurateFormFactor);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Uses tighter radiosity threshold for smoother indirect lighting.\nIncreases compile time.");

    ImGui::Checkbox("CPU-only lighting (no GPU acceleration)", &m_config.cpuOnlyLighting);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Forces VRAD to use CPU ray tracing instead of GPU.\nUse if you experience GPU driver issues.");

    ImGui::Spacing();
    ImGui::Text("Extra VRAD parameters:");
    char buf[256];
    strncpy(buf, m_config.extraRadParams.c_str(), sizeof(buf));
    buf[sizeof(buf) - 1] = '\0';
    ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x);
    if (ImGui::InputText("##extrarad", buf, sizeof(buf)))
        m_config.extraRadParams = buf;

    ImGui::Unindent(10.0f);
}

// ---------------------------------------------------------------------------
void RunMapDialog::DrawGameOptionsSection()
{
    ImGui::Checkbox("Don't run the game after compiling", &m_config.dontRunGame);

    if (m_config.dontRunGame)
        ImGui::BeginDisabled();
    ImGui::Checkbox("Launch with Visual Studio debugger", &m_config.launchWithDebugger);
    if (m_config.dontRunGame)
        ImGui::EndDisabled();

    ImGui::Text("Additional game parameters:");
    char buf[256];
    strncpy(buf, m_config.gameParams.c_str(), sizeof(buf));
    buf[sizeof(buf) - 1] = '\0';
    ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x);
    if (ImGui::InputText("##gameparms", buf, sizeof(buf)))
        m_config.gameParams = buf;
}

// ---------------------------------------------------------------------------
void RunMapDialog::DrawProgressBar()
{
    float progress = m_process->GetProgress();
    std::string stepDesc = m_process->GetCurrentStep();

    char overlay[128];
    snprintf(overlay, sizeof(overlay), "%d%% - %s",
        (int)(progress * 100.0f), stepDesc.c_str());

    ImGui::ProgressBar(progress, ImVec2(-1, 0), overlay);
}

// ---------------------------------------------------------------------------
void RunMapDialog::DrawButtons(ConsolePanel* console)
{
    bool isCompiling = m_process && m_process->IsRunning();

    if (!isCompiling)
    {
        bool canCompile = !m_vmfPath.empty();

        if (!canCompile)
            ImGui::BeginDisabled();

        if (ImGui::Button("Compile", ImVec2(120, 0)))
        {
            m_config.SaveToFile(GetConfigFilePath());
            StartCompile(console);
        }

        if (!canCompile)
            ImGui::EndDisabled();

        ImGui::SameLine();
        if (ImGui::Button("Close", ImVec2(120, 0)))
        {
            m_config.SaveToFile(GetConfigFilePath());
            ImGui::CloseCurrentPopup();
        }
    }
    else
    {
        if (ImGui::Button("Cancel", ImVec2(120, 0)))
            m_process->Cancel();
    }
}

// ---------------------------------------------------------------------------
void RunMapDialog::StartCompile(ConsolePanel* console)
{
    // Validate paths
    if (m_vmfPath.empty())
    {
        console->AddLog("ERROR: No VMF file to compile.");
        return;
    }

    if (m_gameDir.empty())
    {
        console->AddLog("WARNING: Game directory not set. BSP copy and game launch may fail.");
    }

    // Reset state for new compile
    m_leakDetected = false;
    m_process.reset();

    m_process = std::make_unique<CompileProcess>();
    m_process->BuildSteps(m_config, m_vmfPath, m_gameDir);

    if (m_process->GetStepCount() == 0)
    {
        console->AddLog("No compile steps configured.");
        m_process.reset();
        return;
    }

    console->AddLog("Starting compile for: %s", m_vmfPath.c_str());
    m_process->Start();
}

// ---------------------------------------------------------------------------
void RunMapDialog::BrowseForExe(std::string& target)
{
    std::string path = FileDialog::OpenFile(
        "Executables\0*.exe\0All Files\0*.*\0", "exe");
    if (!path.empty())
        target = path;
}

// ---------------------------------------------------------------------------
std::string RunMapDialog::GetConfigFilePath() const
{
    // Store config next to the editor executable
    char exePath[MAX_PATH];
    GetModuleFileNameA(nullptr, exePath, MAX_PATH);
    fs::path p(exePath);
    return (p.parent_path() / "compile_config.ini").string();
}
