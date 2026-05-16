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

constexpr int DISP_TRI_TAG_WALKABLE = (1 << 0);
constexpr int DISP_TRI_TAG_FORCE_WALKABLE_BIT = (1 << 1);
constexpr int DISP_TRI_TAG_FORCE_WALKABLE_VAL = (1 << 2);
constexpr int DISP_TRI_TAG_BUILDABLE = (1 << 3);
constexpr int DISP_TRI_TAG_FORCE_BUILDABLE_BIT = (1 << 4);
constexpr int DISP_TRI_TAG_FORCE_BUILDABLE_VAL = (1 << 5);
constexpr int DISP_TRI_TAG_FORCE_REMOVE_BIT = (1 << 6);

BrushMesh BuildBrushMesh(const VmfSolid& solid);

// Compute the clipped polygon for a given side of a solid, in GL space.
// Returns the polygon vertices (typically 4 for displacement-capable faces), or empty on failure.
std::vector<Vec3> ComputeSidePolygon(const VmfSolid& solid, int sideIndex);

// Compute the outward parent-face normal for a displacement-capable side, in GL space.
// Returns {0,0,0} on failure.
Vec3 ComputeDispFaceNormalGL(const VmfSolid& solid, int sideIndex);

// Compute the GL-space base positions for a displacement face before field-vector distance
// is applied. This is the Source/Hammer "flat + elevation + subdivision offset" position.
std::vector<Vec3> ComputeDispBaseGridPositions(const VmfSolid& solid, int sideIndex);

// Compute the GL-space grid positions for a displacement face.
// Also fills quadVertsGL with the 4 corners of the parent brush face (in GL space).
// Returns gridSize*gridSize positions, or empty if not a valid displacement face.
std::vector<Vec3> ComputeDispGridPositions(const VmfSolid& solid, int sideIndex, std::vector<Vec3>* quadVertsGL = nullptr);

// Store an edited GL-space vertex position back into VMF displacement field-vector data.
// This mirrors Hammer's PaintPosition_Update: target - flat - elevation - offset is
// normalized into normals[index] and distances[index].
bool SetDispVertexPositionGL(VmfSolid& solid, int sideIndex, int vertexIndex, const Vec3& targetGL);

// Recompute walkable/buildable triangle tags from current displacement geometry while
// preserving the explicit forced bits.
void UpdateDispTriangleTags(VmfSolid& solid, int sideIndex);
