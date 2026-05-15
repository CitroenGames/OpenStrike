#pragma once

#include "openstrike/source/source_fgd.hpp"

#include <string>
#include <vector>

class FgdManager
{
public:
    bool LoadFGD(const char* path);
    void ClearFGD();

    const openstrike::SourceFgdEntityClass* FindClass(const char* classname) const;
    const openstrike::SourceFgdGameData* GetGameData() const { return &m_gameData; }
    int GetClassCount() const { return static_cast<int>(m_gameData.classes().size()); }

    bool IsLoaded() const { return m_loaded; }
    const std::vector<std::string>& GetPaths() const { return m_paths; }

private:
    openstrike::SourceFgdGameData m_gameData;
    bool m_loaded = false;
    std::vector<std::string> m_paths;
};
