#include "fgd_manager.hpp"

#include "editor_profiler.hpp"

bool FgdManager::LoadFGD(const char* path)
{
    EDITOR_PROFILE_SCOPE("FgdManager::LoadFGD");
    if (!path || !path[0])
        return false;

    if (!m_gameData.load_file(path))
        return false;

    m_loaded = true;
    m_paths.push_back(path);
    return true;
}

void FgdManager::ClearFGD()
{
    m_gameData.clear();
    m_paths.clear();
    m_loaded = false;
}

const openstrike::SourceFgdEntityClass* FgdManager::FindClass(const char* classname) const
{
    if (!m_loaded || !classname)
        return nullptr;
    return m_gameData.find_class(classname);
}
