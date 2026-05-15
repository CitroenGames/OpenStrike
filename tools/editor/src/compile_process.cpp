#include "compile_process.hpp"

#include <filesystem>
#include <algorithm>

#ifdef _WIN32
#include <windows.h>
#endif

namespace fs = std::filesystem;

CompileProcess::CompileProcess() = default;

CompileProcess::~CompileProcess()
{
    m_cancelled = true;
    if (m_thread.joinable())
        m_thread.join();
}

// ---------------------------------------------------------------------------
// Build the ordered list of compile steps from settings
// ---------------------------------------------------------------------------
void CompileProcess::BuildSteps(const CompileConfig& config,
                                const std::string& vmfPath,
                                const std::string& gameDir)
{
    m_steps.clear();
    m_vmfPath = vmfPath;
    m_gameDir = gameDir;

    // Strip trailing separators to prevent command line escaping issues
    // e.g. -game "D:\path\csgo\" has the backslash escape the closing quote
    while (!m_gameDir.empty() && (m_gameDir.back() == '\\' || m_gameDir.back() == '/'))
        m_gameDir.pop_back();

    std::string dir  = GetDirectory(vmfPath);
    std::string name = GetFilenameNoExt(vmfPath);
    std::string bspPath = dir + "\\" + name + ".bsp";

    // BSP
    if (config.bspMode != BspMode::None && !config.vbspPath.empty())
    {
        CompileStep s;
        s.description = "Running VBSP";
        s.executable  = config.vbspPath;
        s.arguments   = "-game \"" + gameDir + "\" ";
        if (config.bspMode == BspMode::OnlyEntities)
            s.arguments += "-onlyents ";
        s.arguments += "\"" + vmfPath + "\"";
        m_steps.push_back(s);
    }

    // VIS
    if (config.visMode != CompileMode::None && !config.vvisPath.empty())
    {
        CompileStep s;
        s.description = "Running VVIS";
        s.executable  = config.vvisPath;
        s.arguments   = "-game \"" + gameDir + "\" ";
        if (config.visMode == CompileMode::Fast)
            s.arguments += "-fast ";
        s.arguments += "\"" + dir + "\\" + name + "\"";
        m_steps.push_back(s);
    }

    // RAD
    if (config.radMode != CompileMode::None && !config.vradPath.empty())
    {
        CompileStep s;
        s.description = "Running VRAD";
        s.executable  = config.vradPath;
        if (config.hdrLight)
            s.arguments = "-both ";
        s.arguments += "-game \"" + gameDir + "\" ";

        if (config.radMode == CompileMode::Final)
            s.arguments += "-final ";
        else if (config.radMode == CompileMode::Fast)
            s.arguments += "-fast ";

        // Advanced lighting options — only emit when non-default
        if (config.aoRadius != 48.0f)
            s.arguments += "-aoradius " + std::to_string(config.aoRadius) + " ";
        if (config.aoSamples != 48)
            s.arguments += "-aosamples " + std::to_string(config.aoSamples) + " ";
        if (config.sunSamples != 300)
            s.arguments += "-sunsamples " + std::to_string(config.sunSamples) + " ";
        if (config.sampleNormalBias != 0.25f)
            s.arguments += "-samplenormalbias " + std::to_string(config.sampleNormalBias) + " ";
        if (config.staticPropFudge != 1.0f)
            s.arguments += "-staticpropfudge " + std::to_string(config.staticPropFudge) + " ";
        if (config.accurateFormFactor)
            s.arguments += "-accurateformfactor ";
        if (config.cpuOnlyLighting)
            s.arguments += "-cpuonly ";
        if (!config.extraRadParams.empty())
            s.arguments += config.extraRadParams + " ";

        s.arguments += "\"" + dir + "\\" + name + "\"";
        m_steps.push_back(s);
    }

    // Copy BSP to game maps directory
    {
        CompileStep s;
        s.description = "Copying BSP";
        s.executable  = "COPY";
        s.arguments   = bspPath + "|" + gameDir + "\\maps\\" + name + ".bsp";
        s.isSpecial   = true;
        m_steps.push_back(s);
    }

    // Launch game
    if (!config.dontRunGame && !config.gameExePath.empty())
    {
        CompileStep s;
        s.description    = "Launching game";
        s.executable     = config.gameExePath;
        s.arguments      = "-game \"" + gameDir + "\" " + config.gameParams + " +map " + name;
        s.isSpecial      = true;
        s.debuggerAttach = config.launchWithDebugger;
        m_steps.push_back(s);
    }
}

// ---------------------------------------------------------------------------
void CompileProcess::Start()
{
    if (m_running.load())
        return;

    m_cancelled = false;
    m_finished  = false;
    m_currentStep = 0;

    {
        std::lock_guard<std::mutex> lock(m_lineMutex);
        m_pendingLines.clear();
        m_currentStepDesc.clear();
    }

    m_thread = std::thread(&CompileProcess::WorkerThread, this);
}

void CompileProcess::Cancel()
{
    m_cancelled = true;
}

std::vector<std::string> CompileProcess::DrainLogLines()
{
    std::lock_guard<std::mutex> lock(m_lineMutex);
    std::vector<std::string> lines;
    lines.swap(m_pendingLines);
    return lines;
}

std::string CompileProcess::GetCurrentStep() const
{
    std::lock_guard<std::mutex> lock(m_lineMutex);
    return m_currentStepDesc;
}

float CompileProcess::GetProgress() const
{
    if (m_steps.empty())
        return 0.0f;
    return (float)m_currentStep.load() / (float)m_steps.size();
}

void CompileProcess::AddLogLine(const std::string& line)
{
    std::lock_guard<std::mutex> lock(m_lineMutex);
    m_pendingLines.push_back(line);
}

// ---------------------------------------------------------------------------
// Worker thread -- runs compile steps sequentially
// ---------------------------------------------------------------------------
void CompileProcess::WorkerThread()
{
    m_running = true;

    for (int i = 0; i < (int)m_steps.size(); i++)
    {
        if (m_cancelled.load())
            break;

        m_currentStep = i;
        const auto& step = m_steps[i];

        {
            std::lock_guard<std::mutex> lock(m_lineMutex);
            m_currentStepDesc = step.description;
        }
        AddLogLine("--- " + step.description + " ---");

        if (step.isSpecial)
        {
            if (step.executable == "COPY")
            {
                auto sep = step.arguments.find('|');
                std::string src = step.arguments.substr(0, sep);
                std::string dst = step.arguments.substr(sep + 1);
                std::error_code ec;
                fs::create_directories(fs::path(dst).parent_path(), ec);
                fs::copy_file(src, dst, fs::copy_options::overwrite_existing, ec);
                if (ec)
                    AddLogLine("ERROR: Copy failed: " + ec.message());
                else
                    AddLogLine("Copied BSP to " + dst);
            }
            else
            {
                // Game launch
#ifdef _WIN32
                std::string cmdLine = "\"" + step.executable + "\" " + step.arguments;
                std::string gameDir = GetDirectory(step.executable);

                AddLogLine("CMD: " + cmdLine);
                AddLogLine("CWD: " + gameDir);

                STARTUPINFOA si = {};
                si.cb = sizeof(si);
                PROCESS_INFORMATION pi = {};

                DWORD creationFlags = CREATE_NEW_CONSOLE;
                if (step.debuggerAttach)
                    creationFlags |= CREATE_SUSPENDED;

                CreateProcessA(nullptr, cmdLine.data(), nullptr, nullptr,
                    FALSE, creationFlags, nullptr,
                    gameDir.empty() ? nullptr : gameDir.c_str(),
                    &si, &pi);

                if (pi.hProcess)
                {
                    if (step.debuggerAttach)
                    {
                        AddLogLine("Attaching Visual Studio debugger (PID " +
                            std::to_string(pi.dwProcessId) + ")...");

                        std::string dbgCmd = "vsjitdebugger.exe -p " +
                            std::to_string(pi.dwProcessId);
                        STARTUPINFOA dbgSi = {};
                        dbgSi.cb = sizeof(dbgSi);
                        PROCESS_INFORMATION dbgPi = {};

                        BOOL dbgOk = CreateProcessA(nullptr, dbgCmd.data(),
                            nullptr, nullptr, FALSE, 0, nullptr, nullptr,
                            &dbgSi, &dbgPi);

                        if (dbgOk && dbgPi.hProcess)
                        {
                            WaitForSingleObject(dbgPi.hProcess, INFINITE);

                            DWORD dbgExit = 0;
                            GetExitCodeProcess(dbgPi.hProcess, &dbgExit);
                            CloseHandle(dbgPi.hProcess);
                            CloseHandle(dbgPi.hThread);

                            if (dbgExit == 0)
                                AddLogLine("Debugger attached. Resuming game process.");
                            else
                                AddLogLine("WARNING: Debugger attach may have failed "
                                    "(exit code " + std::to_string(dbgExit) +
                                    "). Resuming game anyway.");
                        }
                        else
                        {
                            AddLogLine("ERROR: Failed to launch vsjitdebugger.exe. "
                                "Make sure Visual Studio is installed. "
                                "Resuming game without debugger.");
                        }

                        ResumeThread(pi.hThread);
                    }

                    CloseHandle(pi.hProcess);
                    CloseHandle(pi.hThread);
                    AddLogLine("Game launched.");
                }
                else
                {
                    AddLogLine("ERROR: Failed to launch game.");
                }
#endif
            }
        }
        else
        {
            std::string workDir = GetDirectory(step.executable);
            int exitCode = RunProcess(step.executable, step.arguments, workDir);
            AddLogLine("Exit code: " + std::to_string(exitCode));

            if (exitCode != 0)
            {
                AddLogLine("ERROR: " + step.description + " failed (exit code " + std::to_string(exitCode) + ")");
                break;
            }
        }
    }

    // Mark final step for progress
    m_currentStep = (int)m_steps.size();

    AddLogLine("--- Compile finished ---");
    m_running  = false;
    m_finished = true;
}

// ---------------------------------------------------------------------------
// Launch a process with piped stdout/stderr and capture output
// ---------------------------------------------------------------------------
int CompileProcess::RunProcess(const std::string& exe, const std::string& args,
                               const std::string& workingDir)
{
#ifdef _WIN32
    SECURITY_ATTRIBUTES sa = {};
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;

    HANDLE hReadPipe  = nullptr;
    HANDLE hWritePipe = nullptr;
    if (!CreatePipe(&hReadPipe, &hWritePipe, &sa, 0))
    {
        AddLogLine("ERROR: Failed to create pipe.");
        return -1;
    }
    SetHandleInformation(hReadPipe, HANDLE_FLAG_INHERIT, 0);

    STARTUPINFOA si = {};
    si.cb = sizeof(si);
    si.dwFlags    = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
    si.hStdOutput = hWritePipe;
    si.hStdError  = hWritePipe;
    si.wShowWindow = SW_HIDE;

    std::string cmdLine = "\"" + exe + "\" " + args;

    PROCESS_INFORMATION pi = {};
    BOOL ok = CreateProcessA(
        nullptr,
        cmdLine.data(),
        nullptr, nullptr,
        TRUE,
        CREATE_NO_WINDOW,
        nullptr,
        workingDir.empty() ? nullptr : workingDir.c_str(),
        &si, &pi);

    CloseHandle(hWritePipe);

    if (!ok)
    {
        AddLogLine("ERROR: Failed to launch " + exe);
        CloseHandle(hReadPipe);
        return -1;
    }

    // Read output from pipe
    char buf[4096];
    DWORD bytesRead;
    std::string lineBuffer;

    while (ReadFile(hReadPipe, buf, sizeof(buf) - 1, &bytesRead, nullptr) && bytesRead > 0)
    {
        if (m_cancelled.load())
            break;

        buf[bytesRead] = '\0';
        lineBuffer += buf;

        size_t pos;
        while ((pos = lineBuffer.find('\n')) != std::string::npos)
        {
            std::string line = lineBuffer.substr(0, pos);
            if (!line.empty() && line.back() == '\r')
                line.pop_back();
            if (!line.empty())
                AddLogLine(line);
            lineBuffer.erase(0, pos + 1);
        }
    }

    if (!lineBuffer.empty())
        AddLogLine(lineBuffer);

    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD exitCode = 0;
    GetExitCodeProcess(pi.hProcess, &exitCode);

    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    CloseHandle(hReadPipe);

    return (int)exitCode;
#else
    AddLogLine("ERROR: Compile not supported on this platform.");
    return -1;
#endif
}

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------
std::string CompileProcess::GetDirectory(const std::string& filePath)
{
    fs::path p(filePath);
    return p.parent_path().string();
}

std::string CompileProcess::GetFilenameNoExt(const std::string& filePath)
{
    fs::path p(filePath);
    return p.stem().string();
}
