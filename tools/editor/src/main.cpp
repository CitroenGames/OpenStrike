#include "application.hpp"
#include "editor_profiler.hpp"

#include <cstdio>

// Request dedicated GPU on hybrid-GPU systems (laptops with integrated + discrete)
#if defined(_WIN32)
extern "C" {
    __declspec(dllexport) unsigned long NvOptimusEnablement = 1;
    __declspec(dllexport) int AmdPowerXpressRequestHighPerformance = 1;
}
#endif

#if defined(_WIN32)
    #include <windows.h>
    int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPSTR lpCmdLine, int nCmdShow)
#else
int main(int argc, char* argv[])
#endif
{
    EditorProfiler::Instance().BeginSession("EditorStartup");
    EditorProfiler::Instance().SetThreadName("Main Thread");

    Application app;
    bool initOk;
    {
        EDITOR_PROFILE_SCOPE("Application::Init");
#if defined(_WIN32)
        initOk = app.Init("OpenStrike Editor", 1600, 900, GetCommandLineA());
#else
        initOk = app.Init("OpenStrike Editor", 1600, 900);
#endif
    }

    if (!initOk)
        return 1;

    // Session stays active through first frame to capture ScanForVPKs;
    // Application::Run() will end the session and flush the trace.
    int result = app.Run();
    app.Shutdown();
    return result;
}
