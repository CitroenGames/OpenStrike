#pragma once

#include "vmf_types.hpp"
#include "keyvalues_parser.hpp"
#include <string>
#include <functional>
#include <memory>

enum class SelectionType { None, WorldSolid, Entity, EntitySolid };

struct Selection
{
    SelectionType type     = SelectionType::None;
    int           index    = -1;
    int           subIndex = -1;
};

using LogFunc = std::function<void(const char*)>;

class FgdManager;

class VmfDocument
{
public:
    bool LoadFromFile(const std::string& path, LogFunc log);
    bool SaveToFile(const std::string& path, LogFunc log) const;
    bool ExportToMapFile(const std::string& path, LogFunc log) const;
    void NewDocument();

    const std::string&            GetFilePath() const  { return m_filePath; }
    void  SetFilePath(const std::string& path)         { m_filePath = path; }
    const VmfWorld&               GetWorld() const     { return m_world; }
    VmfWorld&                     GetWorldMut()        { return m_world; }
    const std::vector<VmfEntity>& GetEntities() const  { return m_entities; }
    std::vector<VmfEntity>&       GetEntitiesMut()     { return m_entities; }

    const Selection& GetSelection() const       { return m_selection; }
    void SetSelection(Selection sel)             { m_selection = sel; }
    void ClearSelection()                        { m_selection = {}; }

    const std::unordered_map<std::string, std::string>* GetSelectedKeyValues() const;
    std::unordered_map<std::string, std::string>* GetSelectedKeyValuesMut();
    const char* GetSelectedClassname() const;

    int GetTotalSolidCount() const;
    int GetEntityCount() const { return (int)m_entities.size(); }

    // Mutation
    int  NextID();
    int  AddEntity(const std::string& classname, const Vec3& origin);
    void ApplyFgdDefaults(int entityIndex, FgdManager* fgd);
    void RemoveEntity(int index);
    void MoveEntity(int index, const Vec3& newOrigin);
    void SetEntityKeyValue(int index, const std::string& key, const std::string& value);

    // Solid (brush) creation
    int  AddWorldSolid(const VmfSolid& solid);
    void RemoveWorldSolid(int index);
    void ResizeWorldSolid(int index, const Vec3& newMins, const Vec3& newMaxs);
    void MoveWorldSolid(int index, const Vec3& delta);
    Vec3 GetWorldSolidCenter(int index) const;
    static VmfSolid CreateBoxSolid(int id, const Vec3& mins, const Vec3& maxs, const std::string& material);
    static void ComputeSolidBoundsGL(const VmfSolid& solid, Vec3& mins, Vec3& maxs);
    int ClipWorldSolid(int index, int axis, float dist, int mode, const std::string& material);
    int HollowWorldSolid(int index, float thickness);

    // Displacement creation
    bool CreateDisplacement(int solidIndex, int sideIndex, int power);

    // Brush conversion (Tie to Entity / Move to World)
    int  TieWorldSolidToEntity(int solidIndex, const std::string& classname);
    void MoveEntitySolidsToWorld(int entityIndex);

    // Dirty tracking
    bool IsDirty() const    { return m_dirty; }
    void SetDirty(bool d)   { m_dirty = d; }
    void MarkDirty()        { m_dirty = true; }

    // Get entity for the current selection (or nullptr)
    VmfEntity* GetSelectedEntity();
    const VmfEntity* GetSelectedEntity() const;

private:
    void ParseWorld(const KVNode* node, LogFunc log);
    void ParseEntity(const KVNode* node, LogFunc log);
    void ParseSolid(const KVNode* node, VmfSolid& out);
    void ParseSide(const KVNode* node, VmfSide& out);

    std::string             m_filePath;
    VmfWorld                m_world;
    std::vector<VmfEntity>  m_entities;
    Selection               m_selection;
    int                     m_nextID = 1;
    bool                    m_dirty = false;
};
