#pragma once

#include "compile_config.hpp"
#include <string>
#include <vector>
#include <thread>
#include <mutex>
#include <atomic>

struct CompileStep
{
    std::string description;
    std::string executable;
    std::string arguments;
    bool        isSpecial      = false;   // true for copy/game launch
    bool        debuggerAttach = false;   // launch suspended + attach VS debugger
};

class CompileProcess
{
public:
    CompileProcess();
    ~CompileProcess();

    void BuildSteps(const CompileConfig& config,
                    const std::string& vmfPath,
                    const std::string& gameDir);

    void Start();
    void Cancel();

    bool IsRunning()   const { return m_running.load(); }
    bool IsFinished()  const { return m_finished.load(); }
    bool WasCancelled() const { return m_cancelled.load(); }

    std::vector<std::string> DrainLogLines();
    std::string GetCurrentStep() const;
    float       GetProgress() const;
    int         GetStepCount() const { return (int)m_steps.size(); }

private:
    void WorkerThread();
    int  RunProcess(const std::string& exe, const std::string& args,
                    const std::string& workingDir);
    void AddLogLine(const std::string& line);

    static std::string GetDirectory(const std::string& filePath);
    static std::string GetFilenameNoExt(const std::string& filePath);

    std::vector<CompileStep> m_steps;
    std::string              m_vmfPath;
    std::string              m_gameDir;

    std::thread       m_thread;
    std::atomic<bool> m_running{false};
    std::atomic<bool> m_finished{false};
    std::atomic<bool> m_cancelled{false};
    std::atomic<int>  m_currentStep{0};

    mutable std::mutex       m_lineMutex;
    std::vector<std::string> m_pendingLines;
    std::string              m_currentStepDesc;
};
