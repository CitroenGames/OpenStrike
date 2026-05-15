#include "editor_ui.hpp"
#include "editor_profiler.hpp"
#include "panels/viewport_panel.hpp"
#include "panels/console_panel.hpp"
#include "panels/properties_panel.hpp"
#include "panels/outliner_panel.hpp"
#include "panels/content_browser_panel.hpp"
#include "panels/entity_tool_panel.hpp"
#include "panels/brush_tool_panel.hpp"
#include "panels/clip_tool_panel.hpp"
#include "panels/displacement_tool_panel.hpp"
#include "panels/model_preview_panel.hpp"
#include "panels/res_editor_panel.hpp"
#include "vmf_document.hpp"
#include "fgd_manager.hpp"
#include "file_dialog.hpp"
#include "run_map_dialog.hpp"

#include <cctype>
#include <filesystem>
#include <fstream>
#include <system_error>

namespace fs = std::filesystem;

namespace
{
std::string NormalizeEditorPath(const char* path)
{
    std::string normalized = path;
    std::error_code ec;
    fs::path fsPath(path);
    if (fsPath.is_relative())
    {
        fs::path abs = fs::absolute(fsPath, ec);
        if (!ec)
            normalized = abs.string();
    }

    while (!normalized.empty() && (normalized.back() == '\\' || normalized.back() == '/'))
        normalized.pop_back();

    return normalized;
}

bool ResolveGameDirPath(ConsolePanel* console, const char* path, const char* cmdLine, std::string& outPath)
{
    if (cmdLine)
        console->AddLog("Command line: %s", cmdLine);

    console->AddLog("Game dir argument: %s", path ? path : "(null)");

    if (!path || !path[0])
    {
        console->AddLog("No -game argument provided");
        outPath.clear();
        return false;
    }

    outPath = NormalizeEditorPath(path);
    console->AddLog("Setting game directory: %s", outPath.c_str());
    return true;
}

void ApplyGameDirToPanels(ViewportPanel* viewport, ContentBrowserPanel* contentBrowser, const std::string& gameDir)
{
    { EDITOR_PROFILE_SCOPE("Viewport::SetGameDir"); viewport->SetGameDir(gameDir.c_str()); }
    { EDITOR_PROFILE_SCOPE("ContentBrowser::SetGameDir"); contentBrowser->SetGameDir(gameDir.c_str()); }
}
}

EditorUI::EditorUI() = default;
EditorUI::~EditorUI() = default;

void EditorUI::Init(SDL_Window* window, Rhi* rhi)
{
    EDITOR_PROFILE_SCOPE("EditorUI::Init");

    m_window = window;
    m_viewport   = std::make_unique<ViewportPanel>();
    m_console    = std::make_unique<ConsolePanel>();
    m_properties = std::make_unique<PropertiesPanel>();
    m_outliner   = std::make_unique<OutlinerPanel>();
    m_contentBrowser = std::make_unique<ContentBrowserPanel>();
    m_entityTool = std::make_unique<EntityToolPanel>();
    m_brushTool  = std::make_unique<BrushToolPanel>();
    m_clipTool   = std::make_unique<ClipToolPanel>();
    m_dispTool   = std::make_unique<DisplacementToolPanel>();
    m_modelPreview = std::make_unique<ModelPreviewPanel>();
    m_resEditor  = std::make_unique<ResEditorPanel>();
    m_fgd        = std::make_unique<FgdManager>();
    m_runMapDialog = std::make_unique<RunMapDialog>();

    { EDITOR_PROFILE_SCOPE("ViewportPanel::Init");       m_viewport->Init(rhi); }
    { EDITOR_PROFILE_SCOPE("ConsolePanel::Init");         m_console->Init(); }
    { EDITOR_PROFILE_SCOPE("PropertiesPanel::Init");      m_properties->Init(); }
    { EDITOR_PROFILE_SCOPE("OutlinerPanel::Init");        m_outliner->Init(); }
    { EDITOR_PROFILE_SCOPE("ContentBrowserPanel::Init");  m_contentBrowser->Init(rhi); }
    { EDITOR_PROFILE_SCOPE("EntityToolPanel::Init");      m_entityTool->Init(); }
    { EDITOR_PROFILE_SCOPE("BrushToolPanel::Init");       m_brushTool->Init(); }
    { EDITOR_PROFILE_SCOPE("ClipToolPanel::Init");        m_clipTool->Init(); }
    { EDITOR_PROFILE_SCOPE("DisplacementToolPanel::Init"); m_dispTool->Init(); }
    { EDITOR_PROFILE_SCOPE("ModelPreviewPanel::Init");    m_modelPreview->Init(m_viewport->GetModelCache(), rhi); }
    { EDITOR_PROFILE_SCOPE("ResEditorPanel::Init");       m_resEditor->Init(); }

    m_contentBrowser->SetLogFunc([this](const char* msg) { m_console->AddLog("%s", msg); });
    m_contentBrowser->SetModelOpenFunc([this](const std::string& path, const std::string& vpkName) {
        m_modelPreview->Open(path, vpkName);
    });
    m_contentBrowser->SetResOpenFunc([this](const std::string& vpkPath, int archiveIdx) {
        (void)archiveIdx;
        m_console->AddLog("VPK-backed RES open is not wired yet: %s", vpkPath.c_str());
    });

    m_viewport->SetLogFunc([this](const char* msg) { m_console->AddLog("%s", msg); });
    m_viewport->SetEntityTool(m_entityTool.get());
    m_viewport->SetBrushTool(m_brushTool.get());
    m_viewport->SetClipTool(m_clipTool.get());
    m_viewport->SetDispTool(m_dispTool.get());
    m_viewport->SetSceneChangedFunc([this]() { NotifySceneChanged(); });
    m_properties->SetSceneChangedFunc([this]() { NotifySceneChanged(); });

    { EDITOR_PROFILE_SCOPE("RunMapDialog::Init"); m_runMapDialog->Init(); }

    m_console->AddLog("OpenStrike Editor initialized.");
}

void EditorUI::Shutdown()
{
    m_viewport->Shutdown();
    m_console->Shutdown();
    m_properties->Shutdown();
    m_outliner->Shutdown();
    m_contentBrowser->Shutdown();
    m_entityTool->Shutdown();
    m_brushTool->Shutdown();
    m_clipTool->Shutdown();
    m_dispTool->Shutdown();
    m_modelPreview->Shutdown();
    m_resEditor->Shutdown();
}

void EditorUI::NotifySceneChanged()
{
    if (m_document)
        m_document->MarkDirty();
    m_viewport->MarkDirty();
}

void EditorUI::NewFile()
{
    auto doc = std::make_shared<VmfDocument>();
    doc->NewDocument();

    m_document = doc;
    m_viewport->SetDocument(m_document.get(), m_fgd.get());
    m_console->AddLog("New document created.");
}

void EditorUI::OpenFile()
{
    std::string path = FileDialog::OpenFile(
        "VMF Files\0*.vmf\0All Files\0*.*\0", "vmf");
    if (path.empty())
        return;

    auto doc = std::make_shared<VmfDocument>();
    auto* console = m_console.get();
    bool ok = doc->LoadFromFile(path, [console](const char* msg) {
        console->AddLog("%s", msg);
    });

    if (ok)
    {
        m_document = doc;
        m_viewport->SetDocument(m_document.get(), m_fgd.get());
        m_console->AddLog("Loaded successfully.");
    }
    else
    {
        m_console->AddLog("Failed to load VMF file.");
    }
}

void EditorUI::Save()
{
    if (!m_document)
    {
        m_console->AddLog("No document to save.");
        return;
    }

    if (m_document->GetFilePath().empty())
    {
        SaveAs();
        return;
    }

    auto* console = m_console.get();
    if (m_document->SaveToFile(m_document->GetFilePath(), [console](const char* msg) {
        console->AddLog("%s", msg);
    }))
    {
        m_document->SetDirty(false);
    }
}

void EditorUI::SaveAs()
{
    if (!m_document)
    {
        m_console->AddLog("No document to save.");
        return;
    }

    std::string path = FileDialog::SaveFile(
        "VMF Files\0*.vmf\0All Files\0*.*\0", "vmf");
    if (path.empty())
        return;

    auto* console = m_console.get();
    if (m_document->SaveToFile(path, [console](const char* msg) {
        console->AddLog("%s", msg);
    }))
    {
        m_document->SetFilePath(path);
        m_document->SetDirty(false);
    }
}

void EditorUI::ExportMap()
{
    if (!m_document)
    {
        m_console->AddLog("No document to export.");
        return;
    }

    std::string path = FileDialog::SaveFile(
        "MAP Files\0*.map\0All Files\0*.*\0", "map");
    if (path.empty())
        return;

    auto* console = m_console.get();
    m_document->ExportToMapFile(path, [console](const char* msg) {
        console->AddLog("%s", msg);
    });
}

void EditorUI::LoadFGDFile(const char* path)
{
    EDITOR_PROFILE_SCOPE("EditorUI::LoadFGDFile");

    if (!path || !path[0])
        return;

    // Convert relative paths to absolute
    std::string absPath = path;
    std::error_code ec;
    fs::path fsPath(path);
    if (fsPath.is_relative())
    {
        fs::path abs = fs::absolute(fsPath, ec);
        if (!ec)
            absPath = abs.string();
    }

    m_console->AddLog("Loading FGD: %s", absPath.c_str());
    if (m_fgd->LoadFGD(absPath.c_str()))
    {
        m_console->AddLog("FGD loaded: %d classes.", m_fgd->GetClassCount());

        if (m_document)
            m_viewport->SetDocument(m_document.get(), m_fgd.get());
    }
    else
    {
        m_console->AddLog("Failed to load FGD: %s", absPath.c_str());
    }
}

void EditorUI::OpenFGD()
{
    std::string path = FileDialog::OpenFile(
        "FGD Files\0*.fgd\0All Files\0*.*\0", "fgd");
    if (path.empty())
        return;

    m_console->AddLog("Loading FGD: %s", path.c_str());
    if (m_fgd->LoadFGD(path.c_str()))
    {
        m_console->AddLog("FGD loaded: %d classes.", m_fgd->GetClassCount());

        // Refresh viewport if a document is already loaded
        if (m_document)
            m_viewport->SetDocument(m_document.get(), m_fgd.get());
    }
    else
    {
        m_console->AddLog("Failed to load FGD file.");
    }
}

void EditorUI::BrowseGameDir()
{
    std::string path = FileDialog::BrowseFolder("Select Game Directory (e.g. csgo)");
    if (!path.empty())
        SetGameDir(path.c_str(), nullptr);
}

void EditorUI::SetGameDir(const char* path, const char* cmdLine)
{
    EDITOR_PROFILE_SCOPE("EditorUI::SetGameDir");

    std::string gameDir;
    if (!ResolveGameDirPath(m_console.get(), path, cmdLine, gameDir))
        return;

    m_gameDir = gameDir;
    m_console->AddLog("Set game directory: %s", m_gameDir.c_str());
    ApplyGameDirToPanels(m_viewport.get(), m_contentBrowser.get(), m_gameDir);
}

void EditorUI::PrepareGameDir(const char* path, const char* cmdLine)
{
    std::string gameDir;
    if (!ResolveGameDirPath(m_console.get(), path, cmdLine, gameDir))
        return;

    m_gameDir = gameDir;
    m_loadingGameDir.store(true, std::memory_order_release);
}

void EditorUI::FinishSetGameDir()
{
    EDITOR_PROFILE_SCOPE("FinishSetGameDir");

    if (m_gameDir.empty())
        return;

    // Add "MOD" path (fast — VPKs already loaded by the "GAME" call on the background thread)
    m_console->AddLog("Set game directory: %s", m_gameDir.c_str());
    ApplyGameDirToPanels(m_viewport.get(), m_contentBrowser.get(), m_gameDir);
    m_loadingGameDir.store(false, std::memory_order_release);
}

void EditorUI::HandleFileDrop(const char* filePath)
{
    if (!filePath)
        return;

    std::string path(filePath);

    // Check extension (case-insensitive)
    std::string lower = path;
    for (char& c : lower)
        c = (char)tolower((unsigned char)c);

    if (lower.size() > 4 && lower.substr(lower.size() - 4) == ".fbx")
    {
        if (m_gameDir.empty()) {
            m_console->AddLog("Cannot import FBX: no game directory set");
            return;
        }
        m_contentBrowser->ImportFBX(path, m_gameDir);
        m_console->AddLog("Imported FBX: %s", path.c_str());
    }
    else if (lower.size() > 4 && lower.substr(lower.size() - 4) == ".res")
    {
        // If a VPK archive is active in the content browser, import the file into it
        if (m_contentBrowser->GetSelectedArchive() >= 0)
        {
            m_contentBrowser->ImportFileToVPK(path);
        }
        else
        {
            m_resEditor->OpenFile(path, m_console.get());
        }
    }
}

void EditorUI::RunMap()
{
    if (!m_document)
    {
        m_console->AddLog("No document open.");
        return;
    }

    // Auto-save if dirty
    if (m_document->IsDirty())
        Save();

    // Must have a file path to compile
    if (m_document->GetFilePath().empty())
    {
        m_console->AddLog("Please save the document before compiling.");
        return;
    }

    m_runMapDialog->SetDocumentPath(m_document->GetFilePath());
    m_runMapDialog->SetGameDir(m_gameDir);
    m_runMapDialog->Open();
}

void EditorUI::LoadPointfile()
{
    if (!m_document)
    {
        m_console->AddLog("No document open.");
        return;
    }

    std::string linPath;

    // Try to auto-find the .lin file next to the VMF
    const std::string& vmfPath = m_document->GetFilePath();
    if (!vmfPath.empty())
    {
        fs::path p(vmfPath);
        linPath = (p.parent_path() / (p.stem().string() + ".lin")).string();
        if (!fs::exists(linPath))
            linPath.clear();
    }

    // If not found, open a file dialog
    if (linPath.empty())
    {
        linPath = FileDialog::OpenFile(
            "Pointfiles\0*.lin;*.pts\0All Files\0*.*\0", "lin");
        if (linPath.empty())
            return;
    }

    // Parse the pointfile (one "x y z" per line)
    std::ifstream file(linPath);
    if (!file.is_open())
    {
        m_console->AddLog("Failed to open pointfile: %s", linPath.c_str());
        return;
    }

    std::vector<Vec3> pointsGL;
    std::string line;
    while (std::getline(file, line))
    {
        float x, y, z;
        if (sscanf(line.c_str(), "%f %f %f", &x, &y, &z) == 3)
            pointsGL.push_back(SourceToGL(x, y, z));
    }

    if (pointsGL.size() < 2)
    {
        m_console->AddLog("Pointfile has too few points (%d).", (int)pointsGL.size());
        return;
    }

    m_viewport->GetRenderer().SetLeakTrail(pointsGL);
    m_hasLeakTrail = true;
    m_console->AddLog("Loaded pointfile: %s (%d points)", linPath.c_str(), (int)pointsGL.size());
}

void EditorUI::ClearPointfile()
{
    m_viewport->GetRenderer().ClearLeakTrail();
    m_hasLeakTrail = false;
    m_console->AddLog("Pointfile cleared.");
}
