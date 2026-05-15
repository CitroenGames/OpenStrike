#pragma once

#include <SDL3/SDL.h>

#include <string>

class EditorUI;
class Rhi;

struct TitleBarButtonRects
{
    float minBtnX = 0, minBtnY = 0, minBtnW = 0, minBtnH = 0;
    float maxBtnX = 0, maxBtnY = 0, maxBtnW = 0, maxBtnH = 0;
    float closeBtnX = 0, closeBtnY = 0, closeBtnW = 0, closeBtnH = 0;
    float titleBarHeight = 0;
};

class Application
{
public:
    bool Init(const char* title, int width, int height, const char* cmdLine = "");
    void Shutdown();
    int Run();

    SDL_Window* GetWindow() const { return m_window; }
    Rhi* GetRhi() const { return m_rhi; }

    static TitleBarButtonRects s_titleBarRects;

private:
    SDL_Window* m_window = nullptr;
    Rhi* m_rhi = nullptr;
    bool m_running = false;
    EditorUI* m_editorUI = nullptr;
};
