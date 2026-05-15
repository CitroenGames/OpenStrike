#pragma once

class VmfDocument;

class OutlinerPanel
{
public:
    void Init();
    void Shutdown();
    void Draw(VmfDocument* doc);

    bool& IsOpen() { return m_open; }

private:
    bool m_open = true;
};
