#pragma once

#include "ed_math.hpp"
#include <string>
#include <vector>
#include <optional>
#include <unordered_map>
#include <cstdlib>
#include <cstdio>

struct VmfTexAxis
{
    Vec3  axis;
    float shift = 0.0f;
    float scale = 0.25f;
};

struct VmfDispInfo
{
    int   power = 0;              // 2, 3, or 4. Grid size = (2^power + 1)
    Vec3  startPosition;          // Corner anchor in Source coords
    int   flags = 0;
    float elevation = 0.0f;
    int   subdiv = 0;

    // Row-major grids, indexed [row * GridSize() + col]
    std::vector<Vec3>  normals;       // displacement direction per vertex
    std::vector<float> distances;     // displacement magnitude per vertex
    std::vector<Vec3>  offsets;       // absolute offset per vertex
    std::vector<Vec3>  offsetNormals; // offset normal direction per vertex
    std::vector<float> alphas;        // blend alpha per vertex
    std::vector<int>   triangleTags;  // 2*(2^power) per row, (2^power) rows
    int allowedVerts[10] = {};

    int GridSize() const { return (1 << power) + 1; }
};

struct VmfSide
{
    int         id = 0;
    Vec3        planePoints[3];
    std::string material;
    VmfTexAxis  uaxis;
    VmfTexAxis  vaxis;
    int         lightmapScale = 16;
    int         smoothingGroups = 0;
    std::optional<VmfDispInfo> dispinfo;
};

struct VmfSolid
{
    int                   id = 0;
    std::vector<VmfSide>  sides;
};

struct VmfEntity
{
    int         id = 0;
    std::string classname;
    std::unordered_map<std::string, std::string> keyvalues;
    std::vector<VmfSolid> solids;

    Vec3 GetOrigin() const
    {
        Vec3 v;
        auto it = keyvalues.find("origin");
        if (it != keyvalues.end())
            sscanf(it->second.c_str(), "%f %f %f", &v.x, &v.y, &v.z);
        return v;
    }

    const char* GetValue(const char* key, const char* def = "") const
    {
        auto it = keyvalues.find(key);
        return it != keyvalues.end() ? it->second.c_str() : def;
    }
};

struct VmfWorld
{
    int         id = 0;
    std::string classname;
    std::unordered_map<std::string, std::string> keyvalues;
    std::vector<VmfSolid> solids;
};
