#pragma once

#include "ed_math.hpp"
#include "vmf_types.hpp"
#include <vector>
#include <string>

struct BrushVertex
{
    Vec3 pos;
    Vec3 normal;
    float u = 0, v = 0;
};

struct BrushFace
{
    std::vector<BrushVertex> vertices;
    std::string material;
    bool isDisplacement = false;
};

struct BrushMesh
{
    std::vector<BrushFace> faces;
};

BrushMesh BuildBrushMesh(const VmfSolid& solid);

// Compute the clipped polygon for a given side of a solid, in GL space.
// Returns the polygon vertices (typically 4 for displacement-capable faces), or empty on failure.
std::vector<Vec3> ComputeSidePolygon(const VmfSolid& solid, int sideIndex);

// Compute the GL-space grid positions for a displacement face.
// Also fills quadVertsGL with the 4 corners of the parent brush face (in GL space).
// Returns gridSize*gridSize positions, or empty if not a valid displacement face.
std::vector<Vec3> ComputeDispGridPositions(const VmfSolid& solid, int sideIndex, std::vector<Vec3>* quadVertsGL = nullptr);
