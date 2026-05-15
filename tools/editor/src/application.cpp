#include "application.hpp"

#include "editor_profiler.hpp"
#include "editor_theme.hpp"
#include "editor_ui.hpp"
#include "rhi/rhi.hpp"

#include <cstring>
#include <iomanip>
#include <imgui.h>
#include <imgui_impl_sdl3.h>
#include <sstream>
#include <string>
#include <vector>

TitleBarButtonRects Application::s_titleBarRects = {};

namespace
{
std::vector<std::string> SplitCommandLine(const char* cmdLine)
{
    std::vector<std::string> args;
    if (!cmdLine)
        return args;

    std::istringstream stream(cmdLine);
    std::string token;
    while (stream >> std::quoted(token))
        args.push_back(token);
    return args;
}

const char* FindArgValue(const std::vector<std::string>& args, const char* name)
{
    for (std::size_t i = 0; i + 1 < args.size(); ++i)
    {
        if (args[i] == name)
            return args[i + 1].c_str();
    }
    return nullptr;
}

bool HasArg(const std::vector<std::string>& args, const char* name)
{
    for (const std::string& arg : args)
    {
        if (arg == name)
            return true;
    }
    return false;
}
}

bool Application::Init(const char* title, int width, int height, const char* cmdLine)
{
    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS))
        return false;

    const std::vector<std::string> args = SplitCommandLine(cmdLine);
    const bool useVulkan = HasArg(args, "-vulkan");

    SDL_WindowFlags windowFlags = SDL_WINDOW_RESIZABLE | SDL_WINDOW_MAXIMIZED | SDL_WINDOW_OPENGL;
    if (useVulkan)
        windowFlags = SDL_WINDOW_RESIZABLE | SDL_WINDOW_MAXIMIZED | SDL_WINDOW_OPENGL;

    m_window = SDL_CreateWindow(title, width, height, windowFlags);
    if (!m_window)
        return false;

    m_rhi = Rhi::Create(false);
    if (!m_rhi || !m_rhi->Init(m_window))
        return false;

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();

    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    io.ConfigFlags |= ImGuiConfigFlags_ViewportsEnable;

    ApplyUnrealTheme();
    ImGuiStyle& style = ImGui::GetStyle();
    style.WindowRounding = 0.0f;
    style.Colors[ImGuiCol_WindowBg].w = 1.0f;

    m_rhi->ImGuiInit(m_window);

    m_editorUI = new EditorUI();
    m_editorUI->Init(m_window, m_rhi);

    if (const char* gameDir = FindArgValue(args, "-game"))
        m_editorUI->SetGameDir(gameDir, cmdLine);

    for (std::size_t i = 0; i + 1 < args.size(); ++i)
    {
        if (args[i] == "-fgd")
            m_editorUI->LoadFGDFile(args[i + 1].c_str());
    }

    m_running = true;
    return true;
}

void Application::Shutdown()
{
    if (m_editorUI)
    {
        m_editorUI->Shutdown();
        delete m_editorUI;
        m_editorUI = nullptr;
    }

    if (m_rhi)
        m_rhi->ImGuiShutdown();
    ImGui::DestroyContext();

    if (m_rhi)
    {
        m_rhi->Shutdown();
        delete m_rhi;
        m_rhi = nullptr;
    }

    if (m_window)
    {
        SDL_DestroyWindow(m_window);
        m_window = nullptr;
    }

    SDL_Quit();
}

int Application::Run()
{
    bool firstFrame = true;

    while (m_running)
    {
        SDL_Event event;
        while (SDL_PollEvent(&event))
        {
            ImGui_ImplSDL3_ProcessEvent(&event);
            if (event.type == SDL_EVENT_QUIT)
                m_running = false;
            if (event.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED && event.window.windowID == SDL_GetWindowID(m_window))
                m_running = false;
            if (event.type == SDL_EVENT_DROP_FILE && event.drop.data)
            {
                m_editorUI->HandleFileDrop(event.drop.data);
                SDL_free(const_cast<char*>(event.drop.data));
            }
        }

        m_rhi->BeginFrame();
        m_rhi->ImGuiNewFrame();
        ImGui_ImplSDL3_NewFrame();
        ImGui::NewFrame();
        m_editorUI->Draw();

        if (firstFrame)
        {
            firstFrame = false;
            auto& profiler = EditorProfiler::Instance();
            if (profiler.IsActive())
            {
                profiler.EndSession();
                profiler.FlushToFile();
            }
        }

        ImGui::Render();
        ImGuiIO& io = ImGui::GetIO();
        m_rhi->SetViewport(0, 0, static_cast<int>(io.DisplaySize.x), static_cast<int>(io.DisplaySize.y));
        m_rhi->Clear(0.102f, 0.102f, 0.102f, 1.0f);
        m_rhi->ImGuiRender();
        m_rhi->ImGuiUpdatePlatformWindows();
        m_rhi->EndFrame();
    }

    return 0;
}
