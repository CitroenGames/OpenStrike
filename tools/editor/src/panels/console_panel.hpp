#pragma once

#include <vector>
#include <string>

class ConsolePanel
{
public:
    void Init();
    void Shutdown();
    void Draw();

    void AddLog(const char* fmt, ...);
    void Clear();

    bool& IsOpen() { return m_open; }

private:
    void CopyToClipboard();

    std::vector<std::string> m_lines;
    bool m_autoScroll = true;
    bool m_open       = true;
};
