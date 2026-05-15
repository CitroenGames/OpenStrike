#pragma once

#include "compile_config.hpp"
#include "compile_process.hpp"
#include <memory>
#include <string>

class ConsolePanel;

class RunMapDialog
{
public:
    RunMapDialog();
    ~RunMapDialog();

    void Init();
    void Draw(ConsolePanel* console);
    void Update(ConsolePanel* console);
    void Open();

    void SetDocumentPath(const std::string& vmfPath) { m_vmfPath = vmfPath; }
    void SetGameDir(const std::string& gameDir);

    bool IsCompiling() const;
    bool HasLeakDetected() const { return m_leakDetected; }
    void ClearLeakDetected() { m_leakDetected = false; }

private:
    void DrawToolPathsSection();
    void DrawCompileOptionsSection();
    void DrawAdvancedRadSection();
    void DrawGameOptionsSection();
    void DrawButtons(ConsolePanel* console);
    void DrawProgressBar();
    void StartCompile(ConsolePanel* console);
    void BrowseForExe(std::string& target);

    std::string GetConfigFilePath() const;

    CompileConfig                   m_config;
    std::unique_ptr<CompileProcess> m_process;
    std::string                     m_vmfPath;
    std::string                     m_gameDir;
    bool                            m_wantOpen = false;
    bool                            m_leakDetected = false;
};
