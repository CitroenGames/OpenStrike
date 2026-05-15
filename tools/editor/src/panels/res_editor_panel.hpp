#pragma once

#include <string>

class ConsolePanel;
class CPackedStore;

class ResEditorPanel
{
public:
    void Init();
    void Shutdown();
    void Draw();

    void OpenFile(const std::string& path, ConsolePanel* console);
    void OpenFromVPK(const std::string& content, const std::string& vpkPath, CPackedStore* store, ConsolePanel* console);

    bool& IsOpen() { return m_open; }

    static int ParsePosValue(const std::string& val, int parentExtent, bool isSize, bool proportional, int propBase);

private:
    std::string m_path;
    bool m_open = false;
};
