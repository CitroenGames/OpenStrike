#include "brush_mesh_builder.hpp"
#include <algorithm>


struct Plane
{
    Vec3  normal;
    float dist;
};

static Plane PlaneFromPoints(const Vec3& a, const Vec3& b, const Vec3& c)
{
    Vec3 n = Normalize(Cross(b - a, c - a));
    return { n, Dot(n, a) };
}

static std::vector<Vec3> ClipPolygon(const std::vector<Vec3>& poly, const Plane& plane)
{
    if (poly.empty())
        return {};

    std::vector<Vec3> out;
    float epsilon = 0.01f;

    for (size_t i = 0; i < poly.size(); i++)
    {
        const Vec3& cur = poly[i];
        const Vec3& next = poly[(i + 1) % poly.size()];

        float curDist  = Dot(plane.normal, cur) - plane.dist;
        float nextDist = Dot(plane.normal, next) - plane.dist;

        bool curInside  = curDist >= -epsilon;
        bool nextInside = nextDist >= -epsilon;

        if (curInside)
            out.push_back(cur);

        if (curInside != nextInside)
        {
            float t = curDist / (curDist - nextDist);
            Vec3 intersect = cur + (next - cur) * t;
            out.push_back(intersect);
        }
    }

    return out;
}

static std::vector<Vec3> MakeInitialPolygon(const Plane& plane, float size)
{
    // Find an axis that isn't parallel to the normal
    Vec3 ref = { 0, 0, 1 };
    if (fabsf(Dot(plane.normal, ref)) > 0.9f)
        ref = { 1, 0, 0 };

    Vec3 u = Normalize(Cross(plane.normal, ref));
    Vec3 v = Cross(plane.normal, u);

    Vec3 center = plane.normal * plane.dist;

    return {
        center + u * size + v * size,
        center - u * size + v * size,
        center - u * size - v * size,
        center + u * size - v * size,
    };
}

static void ComputeFaceUVs(BrushFace& face, const VmfSide& side)
{
    float scaleU = (side.uaxis.scale != 0.0f) ? side.uaxis.scale : 0.25f;
    float scaleV = (side.vaxis.scale != 0.0f) ? side.vaxis.scale : 0.25f;

    for (auto& vert : face.vertices)
    {
        // UV computation must be in Source-engine space
        Vec3 srcPos = GLToSource(vert.pos);

        // Store UVs in texel space; the renderer divides by actual texture dimensions
        vert.u = Dot(srcPos, side.uaxis.axis) / scaleU + side.uaxis.shift;
        vert.v = Dot(srcPos, side.vaxis.axis) / scaleV + side.vaxis.shift;
    }
}

static void BuildDisplacementFace(
    const std::vector<Vec3>& quadVerts,
    const VmfSide& side,
    BrushMesh& outMesh)
{
    const VmfDispInfo& disp = side.dispinfo.value();
    int gridSize = disp.GridSize();
    int cells = gridSize - 1;

    // Find which quad corner matches startposition
    Vec3 startGL = SourceToGL(disp.startPosition);
    int startCorner = 0;
    float bestDist = 1e30f;
    for (int i = 0; i < 4; i++)
    {
        Vec3 diff = quadVerts[i] - startGL;
        float d = Dot(diff, diff);
        if (d < bestDist)
        {
            bestDist = d;
            startCorner = i;
        }
    }

    // Order corners CCW from start corner (matching Hammer's GenerateDispSurf layout)
    // corners[0]=start (row0,col0), corners[1]=next CCW (rowMax,col0),
    // corners[2]=opposite (rowMax,colMax), corners[3]=prev CCW (row0,colMax)
    Vec3 corners[4];
    for (int i = 0; i < 4; i++)
        corners[i] = quadVerts[(startCorner + i) % 4];

    // Compute outward surface normal from corners (matching Hammer's GetNormal:
    // Cross(P3-P0, P1-P0) in builddisp.h:448-452)
    Vec3 surfNormal = Normalize(Cross(corners[3] - corners[0], corners[1] - corners[0]));

    // Elevation: uniform offset along surface normal (Hammer's GenerateDispSurf:2028-2031)
    Vec3 elevOffset = surfNormal * disp.elevation;

    // Bilinear interpolation matching Hammer's GenerateDispSurf:
    //   row i walks edge corners[0]->corners[1], col j walks across to corners[3]->corners[2]
    std::vector<Vec3> gridPos(gridSize * gridSize);
    float invSize = 1.0f / (float)(gridSize - 1);

    for (int row = 0; row < gridSize; row++)
    {
        float t = row * invSize;
        for (int col = 0; col < gridSize; col++)
        {
            float s = col * invSize;
            int idx = row * gridSize + col;

            Vec3 basePos =
                corners[0] * (1.0f - t) * (1.0f - s) +
                corners[1] * t          * (1.0f - s) +
                corners[2] * t          * s +
                corners[3] * (1.0f - t) * s;

            Vec3 dispNormal = { 0, 0, 0 };
            float dispDist = 0.0f;
            Vec3 dispOffset = { 0, 0, 0 };

            if (idx < (int)disp.normals.size())
                dispNormal = SourceToGL(disp.normals[idx]);
            if (idx < (int)disp.distances.size())
                dispDist = disp.distances[idx];
            if (idx < (int)disp.offsets.size())
                dispOffset = SourceToGL(disp.offsets[idx]);

            gridPos[idx] = basePos + elevOffset + dispNormal * dispDist + dispOffset;
        }
    }

    // Compute per-vertex normals by averaging adjacent triangle normals
    std::vector<Vec3> gridNormals(gridSize * gridSize, {0, 0, 0});

    for (int row = 0; row < cells; row++)
    {
        for (int col = 0; col < cells; col++)
        {
            int tl = row * gridSize + col;
            int tr = row * gridSize + col + 1;
            int bl = (row + 1) * gridSize + col;
            int br = (row + 1) * gridSize + col + 1;

            // Triangle 1: tl, bl, tr
            {
                Vec3 n = Normalize(Cross(gridPos[bl] - gridPos[tl], gridPos[tr] - gridPos[tl]));
                gridNormals[tl] += n;
                gridNormals[bl] += n;
                gridNormals[tr] += n;
            }
            // Triangle 2: tr, bl, br
            {
                Vec3 n = Normalize(Cross(gridPos[bl] - gridPos[tr], gridPos[br] - gridPos[tr]));
                gridNormals[tr] += n;
                gridNormals[bl] += n;
                gridNormals[br] += n;
            }
        }
    }

    for (auto& n : gridNormals)
        n = Normalize(n);

    // UV computation setup
    float scaleU = (side.uaxis.scale != 0.0f) ? side.uaxis.scale : 0.25f;
    float scaleV = (side.vaxis.scale != 0.0f) ? side.vaxis.scale : 0.25f;

    auto makeVertex = [&](int idx) -> BrushVertex {
        BrushVertex v;
        v.pos = gridPos[idx];
        v.normal = gridNormals[idx];
        Vec3 srcPos = GLToSource(v.pos);
        v.u = Dot(srcPos, side.uaxis.axis) / scaleU + side.uaxis.shift;
        v.v = Dot(srcPos, side.vaxis.axis) / scaleV + side.vaxis.shift;
        return v;
    };

    // Emit each triangle as an individual 3-vertex BrushFace
    // (renderer fan-triangulates each face; 3-vertex faces produce exactly one triangle)
    for (int row = 0; row < cells; row++)
    {
        for (int col = 0; col < cells; col++)
        {
            int tl = row * gridSize + col;
            int tr = row * gridSize + col + 1;
            int bl = (row + 1) * gridSize + col;
            int br = (row + 1) * gridSize + col + 1;

            {
                BrushFace f;
                f.material = side.material;
                f.isDisplacement = true;
                f.vertices.push_back(makeVertex(tl));
                f.vertices.push_back(makeVertex(bl));
                f.vertices.push_back(makeVertex(tr));
                outMesh.faces.push_back(std::move(f));
            }
            {
                BrushFace f;
                f.material = side.material;
                f.isDisplacement = true;
                f.vertices.push_back(makeVertex(tr));
                f.vertices.push_back(makeVertex(bl));
                f.vertices.push_back(makeVertex(br));
                outMesh.faces.push_back(std::move(f));
            }
        }
    }
}

// Compute the clipped quad polygon for a given side of a solid, in GL space
std::vector<Vec3> ComputeSidePolygon(const VmfSolid& solid, int sideIndex)
{
    if (solid.sides.size() < 4 || sideIndex < 0 || sideIndex >= (int)solid.sides.size())
        return {};

    std::vector<Plane> planes;
    planes.reserve(solid.sides.size());
    for (const auto& side : solid.sides)
    {
        planes.push_back(PlaneFromPoints(
            SourceToGL(side.planePoints[0]),
            SourceToGL(side.planePoints[1]),
            SourceToGL(side.planePoints[2])));
    }

    std::vector<Vec3> poly = MakeInitialPolygon(planes[sideIndex], 65536.0f);
    for (size_t j = 0; j < planes.size(); j++)
    {
        if ((int)j == sideIndex)
            continue;
        poly = ClipPolygon(poly, planes[j]);
        if (poly.empty())
            break;
    }
    return poly;
}

std::vector<Vec3> ComputeDispGridPositions(const VmfSolid& solid, int sideIndex, std::vector<Vec3>* quadVertsGL)
{
    if (sideIndex < 0 || sideIndex >= (int)solid.sides.size())
        return {};

    const VmfSide& side = solid.sides[sideIndex];
    if (!side.dispinfo.has_value())
        return {};

    std::vector<Vec3> poly = ComputeSidePolygon(solid, sideIndex);
    if (poly.size() != 4)
        return {};

    if (quadVertsGL)
        *quadVertsGL = poly;

    const VmfDispInfo& disp = side.dispinfo.value();
    int gridSize = disp.GridSize();

    // Find start corner
    Vec3 startGL = SourceToGL(disp.startPosition);
    int startCorner = 0;
    float bestDist = 1e30f;
    for (int i = 0; i < 4; i++)
    {
        Vec3 diff = poly[i] - startGL;
        float d = Dot(diff, diff);
        if (d < bestDist)
        {
            bestDist = d;
            startCorner = i;
        }
    }

    Vec3 corners[4];
    for (int i = 0; i < 4; i++)
        corners[i] = poly[(startCorner + i) % 4];

    // Outward surface normal and elevation offset (matching Hammer's GetNormal + GenerateDispSurf)
    Vec3 surfNormal = Normalize(Cross(corners[3] - corners[0], corners[1] - corners[0]));
    Vec3 elevOffset = surfNormal * disp.elevation;

    std::vector<Vec3> gridPos(gridSize * gridSize);
    float invSize = 1.0f / (float)(gridSize - 1);

    for (int row = 0; row < gridSize; row++)
    {
        float t = row * invSize;
        for (int col = 0; col < gridSize; col++)
        {
            float s = col * invSize;
            int idx = row * gridSize + col;

            // Match Hammer's GenerateDispSurf layout:
            // row walks corners[0]->corners[1], col walks across to corners[3]->corners[2]
            Vec3 basePos =
                corners[0] * (1.0f - t) * (1.0f - s) +
                corners[1] * t          * (1.0f - s) +
                corners[2] * t          * s +
                corners[3] * (1.0f - t) * s;

            Vec3 dispNormal = { 0, 0, 0 };
            float dispDist = 0.0f;
            Vec3 dispOffset = { 0, 0, 0 };

            if (idx < (int)disp.normals.size())
                dispNormal = SourceToGL(disp.normals[idx]);
            if (idx < (int)disp.distances.size())
                dispDist = disp.distances[idx];
            if (idx < (int)disp.offsets.size())
                dispOffset = SourceToGL(disp.offsets[idx]);

            gridPos[idx] = basePos + elevOffset + dispNormal * dispDist + dispOffset;
        }
    }

    return gridPos;
}

BrushMesh BuildBrushMesh(const VmfSolid& solid)
{
    BrushMesh mesh;

    if (solid.sides.size() < 4)
        return mesh;

    // Compute planes for all sides
    std::vector<Plane> planes;
    planes.reserve(solid.sides.size());
    for (const auto& side : solid.sides)
    {
        planes.push_back(PlaneFromPoints(
            SourceToGL(side.planePoints[0]),
            SourceToGL(side.planePoints[1]),
            SourceToGL(side.planePoints[2])));
    }

    // For each face, clip an initial polygon against all other planes
    for (size_t i = 0; i < planes.size(); i++)
    {
        std::vector<Vec3> poly = MakeInitialPolygon(planes[i], 65536.0f);

        for (size_t j = 0; j < planes.size(); j++)
        {
            if (i == j)
                continue;

            // Clip to interior of brush (VMF normals point inward)
            Plane clipPlane;
            clipPlane.normal = planes[j].normal;
            clipPlane.dist   = planes[j].dist;

            poly = ClipPolygon(poly, clipPlane);
            if (poly.empty())
                break;
        }

        if (poly.size() >= 3)
        {
            // Displacement face: generate subdivided grid mesh
            if (solid.sides[i].dispinfo.has_value() && poly.size() == 4)
            {
                BuildDisplacementFace(poly, solid.sides[i], mesh);
            }
            else
            {
                BrushFace face;
                face.material = solid.sides[i].material;
                face.vertices.reserve(poly.size());
                for (const auto& p : poly)
                    face.vertices.push_back({ p, planes[i].normal * -1.0f, 0, 0 });

                ComputeFaceUVs(face, solid.sides[i]);
                mesh.faces.push_back(std::move(face));
            }
        }
    }

    return mesh;
}
