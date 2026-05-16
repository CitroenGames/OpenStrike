#include "vmf_document.hpp"
#include "brush_mesh_builder.hpp"
#include "fgd_manager.hpp"

#include <fstream>
#include <sstream>
#include <cstdio>
#include <cstring>

static void ParseTexAxis(const char* str, VmfTexAxis& out)
{
    // Format: "[ux uy uz shift] scale"
    sscanf(str, "[%f %f %f %f] %f", &out.axis.x, &out.axis.y, &out.axis.z, &out.shift, &out.scale);
}

// ---- Displacement row parsing helpers ----

static void ParseFloatRow(const char* str, std::vector<float>& out, int count)
{
    const char* p = str;
    for (int i = 0; i < count; i++)
    {
        while (*p == ' ') p++;
        if (!*p) break;
        out.push_back((float)atof(p));
        while (*p && *p != ' ') p++;
    }
}

static void ParseVec3Row(const char* str, std::vector<Vec3>& out, int vertexCount)
{
    const char* p = str;
    for (int i = 0; i < vertexCount; i++)
    {
        Vec3 v;
        for (int c = 0; c < 3; c++)
        {
            while (*p == ' ') p++;
            if (!*p) break;
            (&v.x)[c] = (float)atof(p);
            while (*p && *p != ' ') p++;
        }
        out.push_back(v);
    }
}

static void ParseIntRow(const char* str, std::vector<int>& out, int count)
{
    const char* p = str;
    for (int i = 0; i < count; i++)
    {
        while (*p == ' ') p++;
        if (!*p) break;
        out.push_back(atoi(p));
        while (*p && *p != ' ') p++;
    }
}

static void ParseDispInfo(const KVNode* node, VmfDispInfo& disp)
{
    disp.power     = node->GetInt("power", 3);
    disp.flags     = node->GetInt("flags", 0);
    disp.elevation = node->GetFloat("elevation", 0.0f);
    disp.subdiv    = node->GetInt("subdiv", 0);

    const char* sp = node->GetString("startposition");
    if (sp && *sp)
        sscanf(sp, "[%f %f %f]", &disp.startPosition.x, &disp.startPosition.y, &disp.startPosition.z);

    int gridSize = disp.GridSize();
    int numVerts = gridSize * gridSize;
    int cellsPerRow = (1 << disp.power);

    disp.normals.reserve(numVerts);
    disp.distances.reserve(numVerts);
    disp.offsets.reserve(numVerts);
    disp.offsetNormals.reserve(numVerts);
    disp.alphas.reserve(numVerts);
    disp.triangleTags.reserve(cellsPerRow * cellsPerRow * 2);

    auto parseVec3Block = [&](const char* blockName, std::vector<Vec3>& target) {
        const KVNode* block = node->FindChild(blockName);
        if (!block) return;
        for (int row = 0; row < gridSize; row++)
        {
            char rowKey[16];
            snprintf(rowKey, sizeof(rowKey), "row%d", row);
            const char* rowStr = block->GetString(rowKey);
            if (rowStr && *rowStr)
                ParseVec3Row(rowStr, target, gridSize);
        }
    };

    auto parseFloatBlock = [&](const char* blockName, std::vector<float>& target) {
        const KVNode* block = node->FindChild(blockName);
        if (!block) return;
        for (int row = 0; row < gridSize; row++)
        {
            char rowKey[16];
            snprintf(rowKey, sizeof(rowKey), "row%d", row);
            const char* rowStr = block->GetString(rowKey);
            if (rowStr && *rowStr)
                ParseFloatRow(rowStr, target, gridSize);
        }
    };

    parseVec3Block("normals", disp.normals);
    parseFloatBlock("distances", disp.distances);
    parseVec3Block("offsets", disp.offsets);
    parseVec3Block("offset_normals", disp.offsetNormals);
    parseFloatBlock("alphas", disp.alphas);

    // Triangle tags: 2*cellsPerRow triangles per row, cellsPerRow rows
    {
        const KVNode* block = node->FindChild("triangle_tags");
        if (block)
        {
            int triPerRow = 2 * cellsPerRow;
            for (int row = 0; row < cellsPerRow; row++)
            {
                char rowKey[16];
                snprintf(rowKey, sizeof(rowKey), "row%d", row);
                const char* rowStr = block->GetString(rowKey);
                if (rowStr && *rowStr)
                    ParseIntRow(rowStr, disp.triangleTags, triPerRow);
            }
        }
    }

    // Allowed verts
    {
        const KVNode* block = node->FindChild("allowed_verts");
        if (block)
        {
            const char* str = block->GetString("10");
            if (str && *str)
            {
                sscanf(str, "%d %d %d %d %d %d %d %d %d %d",
                    &disp.allowedVerts[0], &disp.allowedVerts[1],
                    &disp.allowedVerts[2], &disp.allowedVerts[3],
                    &disp.allowedVerts[4], &disp.allowedVerts[5],
                    &disp.allowedVerts[6], &disp.allowedVerts[7],
                    &disp.allowedVerts[8], &disp.allowedVerts[9]);
            }
        }
    }
}

void VmfDocument::ParseSide(const KVNode* node, VmfSide& out)
{
    out.id = node->GetInt("id");
    out.material = node->GetString("material");
    out.lightmapScale = node->GetInt("lightmapscale", 16);
    out.smoothingGroups = node->GetInt("smoothing_groups", 0);

    const char* plane = node->GetString("plane");
    if (plane && *plane)
    {
        sscanf(plane, "(%f %f %f) (%f %f %f) (%f %f %f)",
            &out.planePoints[0].x, &out.planePoints[0].y, &out.planePoints[0].z,
            &out.planePoints[1].x, &out.planePoints[1].y, &out.planePoints[1].z,
            &out.planePoints[2].x, &out.planePoints[2].y, &out.planePoints[2].z);
    }

    const char* uaxis = node->GetString("uaxis");
    if (uaxis && *uaxis) ParseTexAxis(uaxis, out.uaxis);

    const char* vaxis = node->GetString("vaxis");
    if (vaxis && *vaxis) ParseTexAxis(vaxis, out.vaxis);

    // Check for displacement info
    const KVNode* dispNode = node->FindChild("dispinfo");
    if (dispNode)
    {
        VmfDispInfo d;
        ParseDispInfo(dispNode, d);
        out.dispinfo = std::move(d);
    }
}

void VmfDocument::ParseSolid(const KVNode* node, VmfSolid& out)
{
    out.id = node->GetInt("id");
    if (out.id >= m_nextID)
        m_nextID = out.id + 1;

    auto sides = node->FindChildren("side");
    for (auto* sideNode : sides)
    {
        VmfSide side;
        ParseSide(sideNode, side);
        if (side.id >= m_nextID)
            m_nextID = side.id + 1;
        out.sides.push_back(std::move(side));
    }
}

void VmfDocument::ParseWorld(const KVNode* node, LogFunc log)
{
    m_world.id = node->GetInt("id");
    if (m_world.id >= m_nextID)
        m_nextID = m_world.id + 1;

    m_world.classname = node->GetString("classname", "worldspawn");

    // Collect all key-value pairs
    for (auto& child : node->children)
    {
        if (!child->IsBlock())
            m_world.keyvalues[child->key] = child->value;
    }

    auto solids = node->FindChildren("solid");
    for (auto* solidNode : solids)
    {
        VmfSolid solid;
        ParseSolid(solidNode, solid);
        m_world.solids.push_back(std::move(solid));
    }

    if (log)
    {
        char buf[128];
        snprintf(buf, sizeof(buf), "  World: %d solids", (int)m_world.solids.size());
        log(buf);
    }
}

void VmfDocument::ParseEntity(const KVNode* node, LogFunc log)
{
    VmfEntity ent;
    ent.id = node->GetInt("id");
    if (ent.id >= m_nextID)
        m_nextID = ent.id + 1;

    for (auto& child : node->children)
    {
        if (!child->IsBlock())
            ent.keyvalues[child->key] = child->value;
    }

    auto it = ent.keyvalues.find("classname");
    if (it != ent.keyvalues.end())
        ent.classname = it->second;

    auto solids = node->FindChildren("solid");
    for (auto* solidNode : solids)
    {
        VmfSolid solid;
        ParseSolid(solidNode, solid);
        ent.solids.push_back(std::move(solid));
    }

    m_entities.push_back(std::move(ent));
}

bool VmfDocument::LoadFromFile(const std::string& path, LogFunc log)
{
    std::ifstream file(path, std::ios::binary);
    if (!file.is_open())
    {
        if (log) log("Failed to open file.");
        return false;
    }

    std::ostringstream ss;
    ss << file.rdbuf();
    std::string content = ss.str();
    file.close();

    if (log)
    {
        char buf[256];
        snprintf(buf, sizeof(buf), "Loading VMF: %s (%d bytes)", path.c_str(), (int)content.size());
        log(buf);
    }

    KVParseResult kv = KV_Parse(content.c_str(), content.size());
    if (!kv.ok)
    {
        if (log)
        {
            char buf[256];
            snprintf(buf, sizeof(buf), "Parse error at line %d: %s", kv.errorLine, kv.error.c_str());
            log(buf);
        }
        return false;
    }

    m_filePath = path;
    m_world = {};
    m_entities.clear();
    m_selection = {};
    m_nextID = 1;
    m_dirty = false;

    for (auto& root : kv.roots)
    {
        if (root->key == "world")
            ParseWorld(root.get(), log);
        else if (root->key == "entity")
            ParseEntity(root.get(), log);
    }

    if (log)
    {
        char buf[128];
        snprintf(buf, sizeof(buf), "  Entities: %d, Total solids: %d",
            GetEntityCount(), GetTotalSolidCount());
        log(buf);
    }

    return true;
}

// ---- VMF Serialization ----

static std::string QuoteString(const std::string& s)
{
    return "\"" + s + "\"";
}

static std::string FormatTexAxis(const VmfTexAxis& axis)
{
    char buf[128];
    snprintf(buf, sizeof(buf), "[%g %g %g %g] %g",
        axis.axis.x, axis.axis.y, axis.axis.z, axis.shift, axis.scale);
    return buf;
}

static std::string FormatPlane(const Vec3 pts[3])
{
    char buf[256];
    snprintf(buf, sizeof(buf), "(%g %g %g) (%g %g %g) (%g %g %g)",
        pts[0].x, pts[0].y, pts[0].z,
        pts[1].x, pts[1].y, pts[1].z,
        pts[2].x, pts[2].y, pts[2].z);
    return buf;
}

static void WriteDispInfo(std::ostream& out, const VmfDispInfo& disp, const std::string& indent)
{
    int gridSize = disp.GridSize();
    int cellsPerRow = (1 << disp.power);
    char buf[128];

    out << indent << "dispinfo\n";
    out << indent << "{\n";
    std::string inner = indent + "\t";
    std::string innerInner = inner + "\t";

    out << inner << "\"power\" \"" << disp.power << "\"\n";
    snprintf(buf, sizeof(buf), "[%g %g %g]",
        disp.startPosition.x, disp.startPosition.y, disp.startPosition.z);
    out << inner << "\"startposition\" \"" << buf << "\"\n";
    out << inner << "\"flags\" \"" << disp.flags << "\"\n";
    out << inner << "\"elevation\" \"" << disp.elevation << "\"\n";
    out << inner << "\"subdiv\" \"" << disp.subdiv << "\"\n";

    auto writeVec3Block = [&](const char* name, const std::vector<Vec3>& data) {
        out << inner << name << "\n" << inner << "{\n";
        for (int row = 0; row < gridSize; row++)
        {
            out << innerInner << "\"row" << row << "\" \"";
            for (int col = 0; col < gridSize; col++)
            {
                int idx = row * gridSize + col;
                if (col > 0) out << " ";
                if (idx < (int)data.size())
                {
                    snprintf(buf, sizeof(buf), "%g %g %g",
                        data[idx].x, data[idx].y, data[idx].z);
                    out << buf;
                }
                else
                    out << "0 0 0";
            }
            out << "\"\n";
        }
        out << inner << "}\n";
    };

    auto writeFloatBlock = [&](const char* name, const std::vector<float>& data) {
        out << inner << name << "\n" << inner << "{\n";
        for (int row = 0; row < gridSize; row++)
        {
            out << innerInner << "\"row" << row << "\" \"";
            for (int col = 0; col < gridSize; col++)
            {
                int idx = row * gridSize + col;
                if (col > 0) out << " ";
                if (idx < (int)data.size())
                {
                    snprintf(buf, sizeof(buf), "%g", data[idx]);
                    out << buf;
                }
                else
                    out << "0";
            }
            out << "\"\n";
        }
        out << inner << "}\n";
    };

    writeVec3Block("normals", disp.normals);
    writeFloatBlock("distances", disp.distances);
    writeVec3Block("offsets", disp.offsets);
    writeVec3Block("offset_normals", disp.offsetNormals);
    writeFloatBlock("alphas", disp.alphas);

    // Triangle tags
    {
        out << inner << "triangle_tags\n" << inner << "{\n";
        int triPerRow = 2 * cellsPerRow;
        for (int row = 0; row < cellsPerRow; row++)
        {
            out << innerInner << "\"row" << row << "\" \"";
            for (int col = 0; col < triPerRow; col++)
            {
                int idx = row * triPerRow + col;
                if (col > 0) out << " ";
                if (idx < (int)disp.triangleTags.size())
                    out << disp.triangleTags[idx];
                else
                    out << "0";
            }
            out << "\"\n";
        }
        out << inner << "}\n";
    }

    // Allowed verts
    {
        out << inner << "allowed_verts\n" << inner << "{\n";
        out << innerInner << "\"10\" \"";
        for (int i = 0; i < 10; i++)
        {
            if (i > 0) out << " ";
            out << disp.allowedVerts[i];
        }
        out << "\"\n";
        out << inner << "}\n";
    }

    out << indent << "}\n";
}

static void WriteSide(std::ostream& out, const VmfSide& side, const std::string& indent)
{
    out << indent << "side\n";
    out << indent << "{\n";
    std::string inner = indent + "\t";
    out << inner << "\"id\" \"" << side.id << "\"\n";
    out << inner << "\"plane\" \"" << FormatPlane(side.planePoints) << "\"\n";
    out << inner << "\"material\" \"" << side.material << "\"\n";
    out << inner << "\"uaxis\" \"" << FormatTexAxis(side.uaxis) << "\"\n";
    out << inner << "\"vaxis\" \"" << FormatTexAxis(side.vaxis) << "\"\n";
    out << inner << "\"lightmapscale\" \"" << side.lightmapScale << "\"\n";
    out << inner << "\"smoothing_groups\" \"" << side.smoothingGroups << "\"\n";
    if (side.dispinfo.has_value())
        WriteDispInfo(out, side.dispinfo.value(), inner);
    out << indent << "}\n";
}

static void WriteSolid(std::ostream& out, const VmfSolid& solid, const std::string& indent)
{
    out << indent << "solid\n";
    out << indent << "{\n";
    std::string inner = indent + "\t";
    out << inner << "\"id\" \"" << solid.id << "\"\n";
    for (const auto& side : solid.sides)
        WriteSide(out, side, inner);
    out << indent << "}\n";
}

static void WriteKeyValues(std::ostream& out,
    const std::unordered_map<std::string, std::string>& kv,
    const std::string& indent)
{
    for (const auto& [k, v] : kv)
        out << indent << QuoteString(k) << " " << QuoteString(v) << "\n";
}

bool VmfDocument::SaveToFile(const std::string& path, LogFunc log) const
{
    std::ofstream file(path, std::ios::binary);
    if (!file.is_open())
    {
        if (log) log("Failed to open file for writing.");
        return false;
    }

    // versioninfo
    file << "versioninfo\n{\n";
    file << "\t\"editorversion\" \"400\"\n";
    file << "\t\"editorbuild\" \"0\"\n";
    file << "\t\"mapversion\" \"1\"\n";
    file << "\t\"formatversion\" \"100\"\n";
    file << "\t\"prefab\" \"0\"\n";
    file << "}\n";

    // visgroups (empty)
    file << "visgroups\n{\n}\n";

    // viewsettings
    file << "viewsettings\n{\n";
    file << "\t\"bSnapToGrid\" \"1\"\n";
    file << "\t\"bShowGrid\" \"1\"\n";
    file << "\t\"nGridSpacing\" \"64\"\n";
    file << "}\n";

    // world
    file << "world\n{\n";
    file << "\t\"id\" \"" << m_world.id << "\"\n";
    // Write world keyvalues (classname, mapversion, etc.)
    for (const auto& [k, v] : m_world.keyvalues)
    {
        if (k == "id") continue; // already written
        file << "\t\"" << k << "\" \"" << v << "\"\n";
    }
    // Ensure classname is present
    if (m_world.keyvalues.find("classname") == m_world.keyvalues.end())
        file << "\t\"classname\" \"worldspawn\"\n";

    for (const auto& solid : m_world.solids)
        WriteSolid(file, solid, "\t");
    file << "}\n";

    // entities
    for (const auto& ent : m_entities)
    {
        file << "entity\n{\n";
        file << "\t\"id\" \"" << ent.id << "\"\n";
        for (const auto& [k, v] : ent.keyvalues)
        {
            if (k == "id") continue;
            file << "\t\"" << k << "\" \"" << v << "\"\n";
        }
        for (const auto& solid : ent.solids)
            WriteSolid(file, solid, "\t");
        file << "}\n";
    }

    file.close();

    if (log)
    {
        char buf[256];
        snprintf(buf, sizeof(buf), "Saved VMF: %s (%d entities, %d world solids)",
            path.c_str(), (int)m_entities.size(), (int)m_world.solids.size());
        log(buf);
    }

    return true;
}

// ---------------------------------------------------------------------------
// .MAP export (Valve 220 format)
// ---------------------------------------------------------------------------

static void WriteMapSide(std::ostream& out, const VmfSide& side, const std::string& indent)
{
    char buf[512];
    const Vec3* p = side.planePoints;
    snprintf(buf, sizeof(buf),
        "%s( %g %g %g ) ( %g %g %g ) ( %g %g %g ) %s "
        "[ %g %g %g %g ] [ %g %g %g %g ] 0 %g %g",
        indent.c_str(),
        p[0].x, p[0].y, p[0].z,
        p[1].x, p[1].y, p[1].z,
        p[2].x, p[2].y, p[2].z,
        side.material.c_str(),
        side.uaxis.axis.x, side.uaxis.axis.y, side.uaxis.axis.z, side.uaxis.shift,
        side.vaxis.axis.x, side.vaxis.axis.y, side.vaxis.axis.z, side.vaxis.shift,
        side.uaxis.scale, side.vaxis.scale);
    out << buf << "\n";
}

static void WriteMapBrush(std::ostream& out, const VmfSolid& solid, const std::string& indent)
{
    out << indent << "{\n";
    std::string inner = indent + "\t";
    for (const auto& side : solid.sides)
        WriteMapSide(out, side, inner);
    out << indent << "}\n";
}

bool VmfDocument::ExportToMapFile(const std::string& path, LogFunc log) const
{
    std::ofstream file(path, std::ios::binary);
    if (!file.is_open())
    {
        if (log) log("Failed to open file for writing.");
        return false;
    }

    bool hasDisplacements = false;

    // worldspawn entity
    file << "{\n";
    // Write world keyvalues
    for (const auto& [k, v] : m_world.keyvalues)
        file << "\"" << k << "\" \"" << v << "\"\n";
    if (m_world.keyvalues.find("classname") == m_world.keyvalues.end())
        file << "\"classname\" \"worldspawn\"\n";
    if (m_world.keyvalues.find("mapversion") == m_world.keyvalues.end())
        file << "\"mapversion\" \"220\"\n";

    for (const auto& solid : m_world.solids)
    {
        for (const auto& side : solid.sides)
            if (side.dispinfo.has_value()) hasDisplacements = true;
        WriteMapBrush(file, solid, "");
    }
    file << "}\n";

    // entities
    for (const auto& ent : m_entities)
    {
        file << "{\n";
        for (const auto& [k, v] : ent.keyvalues)
            file << "\"" << k << "\" \"" << v << "\"\n";
        for (const auto& solid : ent.solids)
        {
            for (const auto& side : solid.sides)
                if (side.dispinfo.has_value()) hasDisplacements = true;
            WriteMapBrush(file, solid, "");
        }
        file << "}\n";
    }

    file.close();

    if (log)
    {
        char buf[256];
        snprintf(buf, sizeof(buf), "Exported MAP: %s (%d entities, %d world solids)",
            path.c_str(), (int)m_entities.size(), (int)m_world.solids.size());
        log(buf);

        if (hasDisplacements)
            log("Warning: Displacements were skipped (not supported in .map format).");
    }

    return true;
}

void VmfDocument::NewDocument()
{
    m_filePath.clear();
    m_world = {};
    m_world.id = 1;
    m_world.classname = "worldspawn";
    m_world.keyvalues["classname"] = "worldspawn";
    m_world.keyvalues["mapversion"] = "1";
    m_entities.clear();
    m_selection = {};
    m_nextID = 2;
    m_dirty = false;
}

// ---- Mutation ----

int VmfDocument::NextID()
{
    return m_nextID++;
}

int VmfDocument::AddEntity(const std::string& classname, const Vec3& origin)
{
    VmfEntity ent;
    ent.id = NextID();
    ent.classname = classname;
    ent.keyvalues["classname"] = classname;

    char buf[128];
    snprintf(buf, sizeof(buf), "%g %g %g", origin.x, origin.y, origin.z);
    ent.keyvalues["origin"] = buf;

    int index = (int)m_entities.size();
    m_entities.push_back(std::move(ent));
    m_dirty = true;
    return index;
}

void VmfDocument::ApplyFgdDefaults(int entityIndex, FgdManager* fgd)
{
    if (!fgd || !fgd->IsLoaded())
        return;
    if (entityIndex < 0 || entityIndex >= (int)m_entities.size())
        return;

    auto& ent = m_entities[entityIndex];
    const openstrike::SourceFgdEntityClass* entityClass = fgd->FindClass(ent.classname.c_str());
    if (!entityClass)
        return;

    for (const openstrike::SourceFgdVariable& variable : entityClass->variables)
    {
        if (variable.name.empty())
            continue;

        // Don't overwrite keys that already exist
        if (variable.name == "classname" || variable.name == "origin")
            continue;
        if (ent.keyvalues.find(variable.name) != ent.keyvalues.end())
            continue;

        std::string value = variable.default_value;
        if (value.empty() && variable.default_integer != 0)
            value = std::to_string(variable.default_integer);

        // Skip empty keys/values and "0" defaults (matches Hammer behavior)
        if (value.empty() || value == "0")
            continue;

        ent.keyvalues[variable.name] = std::move(value);
    }
}

void VmfDocument::RemoveEntity(int index)
{
    if (index < 0 || index >= (int)m_entities.size())
        return;

    // Fix selection
    if (m_selection.type == SelectionType::Entity || m_selection.type == SelectionType::EntitySolid)
    {
        if (m_selection.index == index)
            m_selection = {};
        else if (m_selection.index > index)
            m_selection.index--;
    }

    m_entities.erase(m_entities.begin() + index);
    m_dirty = true;
}

void VmfDocument::MoveEntity(int index, const Vec3& newOrigin)
{
    if (index < 0 || index >= (int)m_entities.size())
        return;

    char buf[128];
    snprintf(buf, sizeof(buf), "%g %g %g", newOrigin.x, newOrigin.y, newOrigin.z);
    m_entities[index].keyvalues["origin"] = buf;
    m_dirty = true;
}

void VmfDocument::SetEntityKeyValue(int index, const std::string& key, const std::string& value)
{
    if (index < 0 || index >= (int)m_entities.size())
        return;

    m_entities[index].keyvalues[key] = value;
    if (key == "classname")
        m_entities[index].classname = value;
    m_dirty = true;
}

// ---- Selection helpers ----

VmfEntity* VmfDocument::GetSelectedEntity()
{
    if (m_selection.type == SelectionType::Entity || m_selection.type == SelectionType::EntitySolid)
    {
        if (m_selection.index >= 0 && m_selection.index < (int)m_entities.size())
            return &m_entities[m_selection.index];
    }
    return nullptr;
}

const VmfEntity* VmfDocument::GetSelectedEntity() const
{
    if (m_selection.type == SelectionType::Entity || m_selection.type == SelectionType::EntitySolid)
    {
        if (m_selection.index >= 0 && m_selection.index < (int)m_entities.size())
            return &m_entities[m_selection.index];
    }
    return nullptr;
}

std::unordered_map<std::string, std::string>* VmfDocument::GetSelectedKeyValuesMut()
{
    switch (m_selection.type)
    {
    case SelectionType::WorldSolid:
        return &m_world.keyvalues;
    case SelectionType::Entity:
    case SelectionType::EntitySolid:
        if (m_selection.index >= 0 && m_selection.index < (int)m_entities.size())
            return &m_entities[m_selection.index].keyvalues;
        break;
    default:
        break;
    }
    return nullptr;
}

const std::unordered_map<std::string, std::string>* VmfDocument::GetSelectedKeyValues() const
{
    switch (m_selection.type)
    {
    case SelectionType::WorldSolid:
        return &m_world.keyvalues;
    case SelectionType::Entity:
    case SelectionType::EntitySolid:
        if (m_selection.index >= 0 && m_selection.index < (int)m_entities.size())
            return &m_entities[m_selection.index].keyvalues;
        break;
    default:
        break;
    }
    return nullptr;
}

const char* VmfDocument::GetSelectedClassname() const
{
    switch (m_selection.type)
    {
    case SelectionType::WorldSolid:
        return m_world.classname.c_str();
    case SelectionType::Entity:
    case SelectionType::EntitySolid:
        if (m_selection.index >= 0 && m_selection.index < (int)m_entities.size())
            return m_entities[m_selection.index].classname.c_str();
        break;
    default:
        break;
    }
    return nullptr;
}

int VmfDocument::GetTotalSolidCount() const
{
    int count = (int)m_world.solids.size();
    for (auto& ent : m_entities)
        count += (int)ent.solids.size();
    return count;
}

// ---- Solid (brush) creation ----

int VmfDocument::AddWorldSolid(const VmfSolid& solid)
{
    VmfSolid copy = solid;
    if (copy.id == 0)
        copy.id = NextID();

    for (auto& side : copy.sides)
    {
        if (side.id == 0)
            side.id = NextID();
    }

    int index = (int)m_world.solids.size();
    m_world.solids.push_back(std::move(copy));
    m_dirty = true;
    return index;
}

void VmfDocument::RemoveWorldSolid(int index)
{
    if (index < 0 || index >= (int)m_world.solids.size())
        return;

    if (m_selection.type == SelectionType::WorldSolid)
    {
        if (m_selection.index == index)
            m_selection = {};
        else if (m_selection.index > index)
            m_selection.index--;
    }

    m_world.solids.erase(m_world.solids.begin() + index);
    m_dirty = true;
}

VmfSolid VmfDocument::CreateBoxSolid(int id, const Vec3& mins, const Vec3& maxs, const std::string& material)
{
    VmfSolid solid;
    solid.id = id;

    float x0 = mins.x, y0 = mins.y, z0 = mins.z;
    float x1 = maxs.x, y1 = maxs.y, z1 = maxs.z;

    auto makeSide = [&](const Vec3& p0, const Vec3& p1, const Vec3& p2,
                        const Vec3& uAxis, const Vec3& vAxis) -> VmfSide
    {
        VmfSide side;
        side.id = 0;
        side.planePoints[0] = p0;
        side.planePoints[1] = p1;
        side.planePoints[2] = p2;
        side.material = material;
        side.uaxis.axis = uAxis;
        side.uaxis.shift = 0.0f;
        side.uaxis.scale = 0.25f;
        side.vaxis.axis = vAxis;
        side.vaxis.shift = 0.0f;
        side.vaxis.scale = 0.25f;
        side.lightmapScale = 16;
        side.smoothingGroups = 0;
        return side;
    };

    // Top face (Z+): normal points down INTO brush
    solid.sides.push_back(makeSide(
        {x0, y0, z1}, {x0, y1, z1}, {x1, y1, z1},
        {1, 0, 0}, {0, -1, 0}
    ));

    // Bottom face (Z-): normal points up INTO brush
    solid.sides.push_back(makeSide(
        {x0, y1, z0}, {x0, y0, z0}, {x1, y0, z0},
        {1, 0, 0}, {0, 1, 0}
    ));

    // Front face (Y+): normal points back INTO brush
    solid.sides.push_back(makeSide(
        {x0, y1, z0}, {x1, y1, z0}, {x1, y1, z1},
        {1, 0, 0}, {0, 0, -1}
    ));

    // Back face (Y-): normal points forward INTO brush
    solid.sides.push_back(makeSide(
        {x1, y0, z0}, {x0, y0, z0}, {x0, y0, z1},
        {-1, 0, 0}, {0, 0, -1}
    ));

    // Right face (X+): normal points left INTO brush
    solid.sides.push_back(makeSide(
        {x1, y1, z0}, {x1, y0, z0}, {x1, y0, z1},
        {0, 1, 0}, {0, 0, -1}
    ));

    // Left face (X-): normal points right INTO brush
    solid.sides.push_back(makeSide(
        {x0, y0, z0}, {x0, y1, z0}, {x0, y1, z1},
        {0, -1, 0}, {0, 0, -1}
    ));

    return solid;
}

// ---- Brush conversion ----

int VmfDocument::TieWorldSolidToEntity(int solidIndex, const std::string& classname)
{
    if (solidIndex < 0 || solidIndex >= (int)m_world.solids.size())
        return -1;

    VmfEntity ent;
    ent.id = NextID();
    ent.classname = classname;
    ent.keyvalues["classname"] = classname;

    ent.solids.push_back(std::move(m_world.solids[solidIndex]));
    m_world.solids.erase(m_world.solids.begin() + solidIndex);

    int entityIndex = (int)m_entities.size();
    m_entities.push_back(std::move(ent));
    m_dirty = true;

    m_selection = { SelectionType::EntitySolid, entityIndex, 0 };

    return entityIndex;
}

void VmfDocument::MoveEntitySolidsToWorld(int entityIndex)
{
    if (entityIndex < 0 || entityIndex >= (int)m_entities.size())
        return;

    VmfEntity& ent = m_entities[entityIndex];
    if (ent.solids.empty())
        return;

    int firstNewIndex = (int)m_world.solids.size();

    for (auto& solid : ent.solids)
        m_world.solids.push_back(std::move(solid));

    m_entities.erase(m_entities.begin() + entityIndex);

    if (m_selection.type == SelectionType::Entity || m_selection.type == SelectionType::EntitySolid)
    {
        if (m_selection.index == entityIndex)
            m_selection = { SelectionType::WorldSolid, firstNewIndex, -1 };
        else if (m_selection.index > entityIndex)
            m_selection.index--;
    }

    m_dirty = true;
}

void VmfDocument::ResizeWorldSolid(int index, const Vec3& newMins, const Vec3& newMaxs)
{
    if (index < 0 || index >= (int)m_world.solids.size())
        return;

    VmfSolid& oldSolid = m_world.solids[index];
    int solidId = oldSolid.id;

    // Get material from first side
    std::string material = "DEV/DEV_MEASUREGENERIC01";
    if (!oldSolid.sides.empty())
        material = oldSolid.sides[0].material;

    // Create new solid with new dimensions
    VmfSolid newSolid = CreateBoxSolid(solidId, newMins, newMaxs, material);

    // Assign new side IDs
    for (auto& side : newSolid.sides)
        side.id = NextID();

    // Replace the old solid
    m_world.solids[index] = std::move(newSolid);
    m_dirty = true;
}

void VmfDocument::MoveWorldSolid(int index, const Vec3& delta)
{
    if (index < 0 || index >= (int)m_world.solids.size())
        return;

    VmfSolid& solid = m_world.solids[index];
    for (auto& side : solid.sides)
    {
        for (int i = 0; i < 3; i++)
        {
            side.planePoints[i].x += delta.x;
            side.planePoints[i].y += delta.y;
            side.planePoints[i].z += delta.z;
        }
    }
    m_dirty = true;
}

Vec3 VmfDocument::GetWorldSolidCenter(int index) const
{
    if (index < 0 || index >= (int)m_world.solids.size())
        return { 0, 0, 0 };

    Vec3 mins, maxs;
    ComputeSolidBoundsGL(m_world.solids[index], mins, maxs);

    // Compute center in GL coords
    Vec3 glCenter = { (mins.x + maxs.x) * 0.5f,
                      (mins.y + maxs.y) * 0.5f,
                      (mins.z + maxs.z) * 0.5f };

    // Convert from GL to Source coords: (x, y, z) -> (x, -z, y)
    return { glCenter.x, -glCenter.z, glCenter.y };
}

void VmfDocument::ComputeSolidBoundsGL(const VmfSolid& solid, Vec3& mins, Vec3& maxs)
{
    BrushMesh mesh = BuildBrushMesh(solid);

    mins = { 1e30f, 1e30f, 1e30f };
    maxs = { -1e30f, -1e30f, -1e30f };

    for (const auto& face : mesh.faces)
    {
        for (const auto& v : face.vertices)
        {
            if (v.pos.x < mins.x) mins.x = v.pos.x;
            if (v.pos.y < mins.y) mins.y = v.pos.y;
            if (v.pos.z < mins.z) mins.z = v.pos.z;
            if (v.pos.x > maxs.x) maxs.x = v.pos.x;
            if (v.pos.y > maxs.y) maxs.y = v.pos.y;
            if (v.pos.z > maxs.z) maxs.z = v.pos.z;
        }
    }
}

// ---- Brush Clipping ----
// axis: 0=X, 1=Y, 2=Z (Source-space axis)
// mode: 0=KeepBoth, 1=KeepFront, 2=KeepBack
// Returns index of first new solid, or -1 on failure

int VmfDocument::ClipWorldSolid(int index, int axis, float dist, int mode, const std::string& material)
{
    if (index < 0 || index >= (int)m_world.solids.size())
        return -1;
    if (axis < 0 || axis > 2)
        return -1;

    const VmfSolid& original = m_world.solids[index];

    // Build two halves: front (positive side) and back (negative side)
    VmfSolid frontSolid, backSolid;
    frontSolid.id = 0;
    backSolid.id = 0;

    // Copy all original sides to both halves
    for (const auto& side : original.sides)
    {
        VmfSide fs = side;
        fs.id = 0;
        frontSolid.sides.push_back(fs);

        VmfSide bs = side;
        bs.id = 0;
        backSolid.sides.push_back(bs);
    }

    // Create the clip plane side for each half
    // Front half gets a cap with inward normal pointing in -axis direction
    // Back half gets a cap with inward normal pointing in +axis direction
    auto makeClipSide = [&](const Vec3& p0, const Vec3& p1, const Vec3& p2,
                            const Vec3& uAxis, const Vec3& vAxis) -> VmfSide
    {
        VmfSide side;
        side.id = 0;
        side.planePoints[0] = p0;
        side.planePoints[1] = p1;
        side.planePoints[2] = p2;
        side.material = material;
        side.uaxis.axis = uAxis;
        side.uaxis.shift = 0.0f;
        side.uaxis.scale = 0.25f;
        side.vaxis.axis = vAxis;
        side.vaxis.shift = 0.0f;
        side.vaxis.scale = 0.25f;
        side.lightmapScale = 16;
        side.smoothingGroups = 0;
        return side;
    };

    // Plane points for each axis, verified winding so Cross(p1-p0, p2-p0) gives correct inward normal
    if (axis == 0) // X-axis clip
    {
        // Front half cap: normal (-1,0,0) - points inward toward front half
        frontSolid.sides.push_back(makeClipSide(
            {dist, 0, 0}, {dist, 0, 1}, {dist, 1, 0},
            {0, -1, 0}, {0, 0, -1}
        ));
        // Back half cap: normal (+1,0,0) - points inward toward back half
        backSolid.sides.push_back(makeClipSide(
            {dist, 0, 0}, {dist, 1, 0}, {dist, 0, 1},
            {0, 1, 0}, {0, 0, -1}
        ));
    }
    else if (axis == 1) // Y-axis clip
    {
        // Front half cap: normal (0,-1,0)
        frontSolid.sides.push_back(makeClipSide(
            {0, dist, 0}, {1, dist, 0}, {0, dist, 1},
            {1, 0, 0}, {0, 0, -1}
        ));
        // Back half cap: normal (0,+1,0)
        backSolid.sides.push_back(makeClipSide(
            {0, dist, 0}, {0, dist, 1}, {1, dist, 0},
            {-1, 0, 0}, {0, 0, -1}
        ));
    }
    else // Z-axis clip
    {
        // Front half cap: normal (0,0,-1)
        frontSolid.sides.push_back(makeClipSide(
            {0, 0, dist}, {0, 1, dist}, {1, 0, dist},
            {1, 0, 0}, {0, -1, 0}
        ));
        // Back half cap: normal (0,0,+1)
        backSolid.sides.push_back(makeClipSide(
            {0, 0, dist}, {1, 0, dist}, {0, 1, dist},
            {1, 0, 0}, {0, 1, 0}
        ));
    }

    // Validate both halves produce valid geometry
    BrushMesh frontMesh = BuildBrushMesh(frontSolid);
    BrushMesh backMesh = BuildBrushMesh(backSolid);

    bool frontValid = !frontMesh.faces.empty();
    bool backValid = !backMesh.faces.empty();

    // If neither is valid, do nothing
    if (!frontValid && !backValid)
        return -1;

    // Remove the original solid
    RemoveWorldSolid(index);

    int firstNew = -1;

    // Add kept halves based on mode
    if (mode == 0) // KeepBoth
    {
        if (frontValid)
        {
            firstNew = AddWorldSolid(frontSolid);
        }
        if (backValid)
        {
            int idx = AddWorldSolid(backSolid);
            if (firstNew < 0) firstNew = idx;
        }
    }
    else if (mode == 1) // KeepFront
    {
        if (frontValid)
            firstNew = AddWorldSolid(frontSolid);
    }
    else // KeepBack
    {
        if (backValid)
            firstNew = AddWorldSolid(backSolid);
    }

    // Select the first new solid
    if (firstNew >= 0)
        m_selection = { SelectionType::WorldSolid, firstNew, -1 };

    m_dirty = true;
    return firstNew;
}

// ---- Brush Hollow ----
// Replaces a solid with 6 wall brushes forming a hollow room.
// Returns index of first new solid, or -1 on failure.

int VmfDocument::HollowWorldSolid(int index, float thickness)
{
    if (index < 0 || index >= (int)m_world.solids.size())
        return -1;
    if (thickness <= 0.0f)
        return -1;

    const VmfSolid& original = m_world.solids[index];

    // Get bounds in GL space, convert to Source space
    Vec3 glMins, glMaxs;
    ComputeSolidBoundsGL(original, glMins, glMaxs);

    // GL → Source: (x, y, z) → (x, -z, y)
    Vec3 sMin = { glMins.x, -glMaxs.z, glMins.y };
    Vec3 sMax = { glMaxs.x, -glMins.z, glMaxs.y };

    // Ensure mins < maxs in Source space
    if (sMin.x > sMax.x) { float t = sMin.x; sMin.x = sMax.x; sMax.x = t; }
    if (sMin.y > sMax.y) { float t = sMin.y; sMin.y = sMax.y; sMax.y = t; }
    if (sMin.z > sMax.z) { float t = sMin.z; sMin.z = sMax.z; sMax.z = t; }

    // Validate thickness against smallest dimension
    float dx = sMax.x - sMin.x;
    float dy = sMax.y - sMin.y;
    float dz = sMax.z - sMin.z;
    float minDim = dx < dy ? dx : dy;
    if (dz < minDim) minDim = dz;
    if (thickness * 2.0f >= minDim)
        return -1;

    // Get material from original
    std::string material = "DEV/DEV_MEASUREGENERIC01";
    if (!original.sides.empty())
        material = original.sides[0].material;

    // Remove original solid
    RemoveWorldSolid(index);

    float T = thickness;
    int firstNew = -1;

    // Create 6 wall brushes (non-overlapping)
    struct WallDef { Vec3 mins, maxs; };
    WallDef walls[6] = {
        // Top:    full XY slab at top
        { {sMin.x, sMin.y, sMax.z - T}, {sMax.x, sMax.y, sMax.z} },
        // Bottom: full XY slab at bottom
        { {sMin.x, sMin.y, sMin.z},     {sMax.x, sMax.y, sMin.z + T} },
        // Front (Y+): between top and bottom
        { {sMin.x, sMax.y - T, sMin.z + T}, {sMax.x, sMax.y, sMax.z - T} },
        // Back (Y-): between top and bottom
        { {sMin.x, sMin.y, sMin.z + T},     {sMax.x, sMin.y + T, sMax.z - T} },
        // Right (X+): innermost
        { {sMax.x - T, sMin.y + T, sMin.z + T}, {sMax.x, sMax.y - T, sMax.z - T} },
        // Left (X-): innermost
        { {sMin.x, sMin.y + T, sMin.z + T},     {sMin.x + T, sMax.y - T, sMax.z - T} },
    };

    for (int i = 0; i < 6; i++)
    {
        VmfSolid wall = CreateBoxSolid(NextID(), walls[i].mins, walls[i].maxs, material);
        for (auto& side : wall.sides)
            side.id = NextID();
        int idx = AddWorldSolid(wall);
        if (firstNew < 0) firstNew = idx;
    }

    // Select the first new solid
    if (firstNew >= 0)
        m_selection = { SelectionType::WorldSolid, firstNew, -1 };

    m_dirty = true;
    return firstNew;
}

bool VmfDocument::CreateDisplacement(int solidIndex, int sideIndex, int power)
{
    if (solidIndex < 0 || solidIndex >= (int)m_world.solids.size())
        return false;

    VmfSolid& solid = m_world.solids[solidIndex];
    if (sideIndex < 0 || sideIndex >= (int)solid.sides.size())
        return false;

    if (power < 2 || power > 4)
        return false;

    VmfSide& side = solid.sides[sideIndex];
    if (side.dispinfo.has_value())
        return false; // already a displacement

    // Compute actual face polygon via CSG clipping to get real corners
    std::vector<Vec3> polyGL = ComputeSidePolygon(solid, sideIndex);
    if (polyGL.size() != 4)
        return false; // displacement requires a quad face

    // Convert first corner from GL space back to Source coords for startPosition
    // GLToSource: {v.x, -v.z, v.y}
    Vec3 startGL = polyGL[0];
    Vec3 startSrc = { startGL.x, -startGL.z, startGL.y };

    VmfDispInfo disp;
    disp.power = power;
    disp.startPosition = startSrc;
    disp.flags = 0;
    disp.elevation = 0.0f;
    disp.subdiv = 0;

    int gridSize = disp.GridSize();
    int vertCount = gridSize * gridSize;

    // Compute outward face normal from polygon corners
    // Matches Hammer's GetNormal: Cross(P3-P0, P1-P0) in GL space
    Vec3 surfNormalGL = Normalize(Cross(polyGL[3] - polyGL[0], polyGL[1] - polyGL[0]));
    // Convert to Source coords for storage in VMF
    Vec3 normal = { surfNormalGL.x, -surfNormalGL.z, surfNormalGL.y };

    disp.normals.resize(vertCount, normal);
    disp.distances.resize(vertCount, 0.0f);
    disp.offsets.resize(vertCount, {0, 0, 0});
    disp.offsetNormals.resize(vertCount, normal);
    disp.alphas.resize(vertCount, 0.0f);

    int triCols = (1 << power);
    int triTagCount = triCols * triCols * 2;
    disp.triangleTags.resize(triTagCount, 0);

    for (int i = 0; i < 10; i++)
        disp.allowedVerts[i] = -1; // all allowed

    side.dispinfo = std::move(disp);
    UpdateDispTriangleTags(solid, sideIndex);
    m_dirty = true;
    return true;
}
