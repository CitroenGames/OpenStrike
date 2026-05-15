#pragma once

#include <memory>
#include <string>
#include <atomic>

struct SDL_Window;
class Rhi;

class ViewportPanel;
class ConsolePanel;
class PropertiesPanel;
class OutlinerPanel;
class ContentBrowserPanel;
class EntityToolPanel;
class BrushToolPanel;
class ClipToolPanel;
class DisplacementToolPanel;
class ModelPreviewPanel;
class ResEditorPanel;
class RunMapDialog;
class VmfDocument;
class FgdManager;

class EditorUI
{
public:
    EditorUI();
    ~EditorUI();

    void Init(SDL_Window* window, Rhi* rhi);
    void Shutdown();
    void Draw();
    void SetGameDir(const char* path, const char* cmdLine = nullptr);
    void PrepareGameDir(const char* path, const char* cmdLine = nullptr);
    void FinishSetGameDir();
    bool IsLoadingGameDir() const { return m_loadingGameDir.load(std::memory_order_acquire); }
    const std::string& GetGameDir() const { return m_gameDir; }
    void LoadFGDFile(const char* path);
    void HandleFileDrop(const char* filePath);

private:
    void DrawTitleBar();
    void DrawMenuBar();
    void DrawDockspace();
    void DrawEditorPanels();
    void DrawStartupDialog();
    void DrawModalDialogs();
    void DrawRunMapDialog();
    void HandleKeyboardShortcuts();
    void SetupDefaultLayout(unsigned int dockspaceId);
    void DrawFileMenu();
    void DrawEditMenu();
    void DrawMapMenu();
    void DrawViewMenu();
    void DrawToolsMenu();
    void DrawHelpMenu();
    void OpenFile();
    void OpenFGD();
    void BrowseGameDir();
    void NewFile();
    void Save();
    void SaveAs();
    void NotifySceneChanged();
    void RunMap();
    void LoadPointfile();
    void ClearPointfile();
    void ExportMap();

    SDL_Window* m_window = nullptr;

    std::unique_ptr<ViewportPanel>   m_viewport;
    std::unique_ptr<ConsolePanel>    m_console;
    std::unique_ptr<PropertiesPanel> m_properties;
    std::unique_ptr<OutlinerPanel>   m_outliner;
    std::unique_ptr<ContentBrowserPanel> m_contentBrowser;
    std::unique_ptr<EntityToolPanel> m_entityTool;
    std::unique_ptr<BrushToolPanel>  m_brushTool;
    std::unique_ptr<ClipToolPanel>   m_clipTool;
    std::unique_ptr<DisplacementToolPanel> m_dispTool;
    std::unique_ptr<ModelPreviewPanel> m_modelPreview;
    std::unique_ptr<ResEditorPanel>    m_resEditor;
    std::shared_ptr<VmfDocument>     m_document;
    std::unique_ptr<FgdManager>      m_fgd;
    std::unique_ptr<RunMapDialog>    m_runMapDialog;
    std::string                      m_gameDir;

    bool  m_hasLeakTrail      = false;
    bool  m_showAbout         = false;
    bool  m_firstFrame        = true;
    bool  m_showStartupDialog = true;
    bool  m_showHollowDialog  = false;
    float m_hollowThickness   = 32.0f;
    std::atomic<bool> m_loadingGameDir{false};
};
