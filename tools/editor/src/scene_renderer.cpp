#include "scene_renderer.hpp"
#include "rhi/rhi.hpp"
#include "brush_mesh_builder.hpp"
#include "vmf_document.hpp"
#include "fgd_manager.hpp"
#include "editor_profiler.hpp"

#include <vector>
#include <algorithm>
#include <unordered_map>
#include <unordered_set>
#include <cstdio>
#include <cstdarg>
#include <cctype>
#include <cstdlib>

static std::string ToLower(const std::string& s)
{
    std::string out = s;
    for (auto& c : out) c = (char)std::tolower((unsigned char)c);
    return out;
}

static bool IsToolTexture(const std::string& mat, const char* keyword)
{
    std::string lower = ToLower(mat);
    return lower.find(keyword) != std::string::npos;
}

void SceneRenderer::Log(const char* fmt, ...)
{
    if (!m_logFunc)
        return;
    char buf[512];
    va_list args;
    va_start(args, fmt);
    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    m_logFunc(buf);
}

// Helper to create a vertex layout with pos3+color3 (6 floats), draw as lines, then destroy
void SceneRenderer::DrawTempWireframe(const std::vector<float>& verts, float lineWidth)
{
    if (verts.empty()) return;

    RhiBuffer tmpVBO = m_rhi->CreateBuffer(RhiBufferUsage::Vertex,
                                           verts.data(), verts.size() * sizeof(float));
    std::vector<RhiVertexAttrib> attribs = {
        { 0, 3, 6 * (int)sizeof(float), 0 },
        { 1, 3, 6 * (int)sizeof(float), 3 * (int)sizeof(float) },
    };
    RhiVertexLayout tmpVL = m_rhi->CreateVertexLayout(tmpVBO, attribs);

    m_rhi->SetUniformInt(m_locUseLighting, 0);
    m_rhi->SetUniformInt(m_locUseVertexColor, 1);

    m_rhi->SetLineWidth(lineWidth);
    m_rhi->BindVertexLayout(tmpVL);
    m_rhi->Draw(RhiPrimitive::Lines, 0, (int)(verts.size() / 6));
    m_rhi->SetLineWidth(1.0f);

    m_rhi->SetUniformInt(m_locUseVertexColor, 0);

    m_rhi->DestroyVertexLayout(tmpVL);
    m_rhi->DestroyBuffer(tmpVBO);
}

void SceneRenderer::BuildUnitCube()
{
    float v[] = {
        -1,-1, 1,  1,-1, 1,  1, 1, 1,  -1,-1, 1,  1, 1, 1,  -1, 1, 1,
        -1,-1,-1,  -1, 1,-1,  1, 1,-1,  -1,-1,-1,  1, 1,-1,  1,-1,-1,
        -1,-1,-1,  -1,-1, 1,  -1, 1, 1,  -1,-1,-1,  -1, 1, 1,  -1, 1,-1,
         1,-1,-1,  1, 1,-1,  1, 1, 1,  1,-1,-1,  1, 1, 1,  1,-1, 1,
        -1, 1,-1,  -1, 1, 1,  1, 1, 1,  -1, 1,-1,  1, 1, 1,  1, 1,-1,
        -1,-1,-1,  1,-1,-1,  1,-1, 1,  -1,-1,-1,  1,-1, 1,  -1,-1, 1,
    };

    m_cubeVBO = m_rhi->CreateBuffer(RhiBufferUsage::Vertex, v, sizeof(v));
    std::vector<RhiVertexAttrib> attribs = {
        { 0, 3, 3 * (int)sizeof(float), 0 },
    };
    m_cubeVL = m_rhi->CreateVertexLayout(m_cubeVBO, attribs);
}

void SceneRenderer::BuildGrid()
{
    struct Vert { float x, y, z; };
    std::vector<Vert> verts;

    float gridSize = 2048.0f;
    float gridStep = 64.0f;

    for (float i = -gridSize; i <= gridSize; i += gridStep)
    {
        verts.push_back({ -gridSize, 0, i });
        verts.push_back({  gridSize, 0, i });
        verts.push_back({ i, 0, -gridSize });
        verts.push_back({ i, 0,  gridSize });
    }

    m_gridVertexCount = (int)verts.size();

    m_gridVBO = m_rhi->CreateBuffer(RhiBufferUsage::Vertex,
                                    verts.data(), verts.size() * sizeof(Vert));
    std::vector<RhiVertexAttrib> attribs = {
        { 0, 3, (int)sizeof(Vert), 0 },
    };
    m_gridVL = m_rhi->CreateVertexLayout(m_gridVBO, attribs);
}

bool SceneRenderer::Init(Rhi* rhi)
{
    EDITOR_PROFILE_SCOPE("SceneRenderer::Init");
    m_rhi = rhi;

    { EDITOR_PROFILE_SCOPE("CreateSceneShader");
      m_sceneShader = m_rhi->CreateSceneShader();
    }
    if (!m_sceneShader)
        return false;

    // Cache uniform locations
    m_locMVP            = m_rhi->GetSceneUniformMVP();
    m_locColor          = m_rhi->GetSceneUniformColor();
    m_locUseLighting    = m_rhi->GetSceneUniformUseLighting();
    m_locUseVertexColor = m_rhi->GetSceneUniformUseVertexColor();
    m_locUseTexture     = m_rhi->GetSceneUniformUseTexture();
    m_locTexSampler     = m_rhi->GetSceneUniformTexSampler();

    { EDITOR_PROFILE_SCOPE("CreatePickShader");
      m_pickShader = m_rhi->CreatePickShader();
    }
    m_locPickMVP = m_rhi->GetPickUniformMVP();

    { EDITOR_PROFILE_SCOPE("BuildGrid");         BuildGrid(); }
    { EDITOR_PROFILE_SCOPE("BuildUnitCube");     BuildUnitCube(); }
    { EDITOR_PROFILE_SCOPE("TextureCache::Init"); m_textureCache.Init(m_rhi); }
    { EDITOR_PROFILE_SCOPE("ModelCache::Init");  m_modelCache.Init(m_rhi); }
    return true;
}

void SceneRenderer::Shutdown()
{
    DestroyBrushMeshes();
    DestroyEntityMeshes();
    DestroyRopeMeshes();
    m_modelInstances.clear();

    if (m_gridVL)  { m_rhi->DestroyVertexLayout(m_gridVL); m_gridVL = RHI_NULL_VERTEX_LAYOUT; }
    if (m_gridVBO) { m_rhi->DestroyBuffer(m_gridVBO); m_gridVBO = RHI_NULL_BUFFER; }
    if (m_cubeVL)  { m_rhi->DestroyVertexLayout(m_cubeVL); m_cubeVL = RHI_NULL_VERTEX_LAYOUT; }
    if (m_cubeVBO) { m_rhi->DestroyBuffer(m_cubeVBO); m_cubeVBO = RHI_NULL_BUFFER; }
    m_textureCache.Shutdown();
    m_modelCache.Shutdown();
    ClearLeakTrail();
    if (m_sceneShader) { m_rhi->DestroyShader(m_sceneShader); m_sceneShader = RHI_NULL_SHADER; }
    if (m_pickShader)  { m_rhi->DestroyShader(m_pickShader);  m_pickShader  = RHI_NULL_SHADER; }
}

void SceneRenderer::SetDocument(VmfDocument* doc, FgdManager* fgd)
{
    m_doc = doc;
    m_fgd = fgd;
    m_dirty = true;
}

void SceneRenderer::SetGameDir(const char* dir)
{
    EDITOR_PROFILE_SCOPE("SceneRenderer::SetGameDir");
    m_textureCache.SetGameDir(dir);
    { EDITOR_PROFILE_SCOPE("ModelCache::SetGameDir"); m_modelCache.SetGameDir(dir); }
    m_dirty = true;
}

void SceneRenderer::DestroyBrushMeshes()
{
    if (m_brushVL)   { m_rhi->DestroyVertexLayout(m_brushVL); m_brushVL = RHI_NULL_VERTEX_LAYOUT; }
    if (m_brushVBO)  { m_rhi->DestroyBuffer(m_brushVBO); m_brushVBO = RHI_NULL_BUFFER; }
    m_brushVertexCount = 0;
    m_drawBatches.clear();

    if (m_bspWireVL)  { m_rhi->DestroyVertexLayout(m_bspWireVL); m_bspWireVL = RHI_NULL_VERTEX_LAYOUT; }
    if (m_bspWireVBO) { m_rhi->DestroyBuffer(m_bspWireVBO); m_bspWireVBO = RHI_NULL_BUFFER; }
    m_bspWireVertexCount = 0;

    if (m_dispWireVL)  { m_rhi->DestroyVertexLayout(m_dispWireVL); m_dispWireVL = RHI_NULL_VERTEX_LAYOUT; }
    if (m_dispWireVBO) { m_rhi->DestroyBuffer(m_dispWireVBO); m_dispWireVBO = RHI_NULL_BUFFER; }
    m_dispWireVertexCount = 0;
}

void SceneRenderer::DestroyEntityMeshes()
{
    if (m_entVL)  { m_rhi->DestroyVertexLayout(m_entVL); m_entVL = RHI_NULL_VERTEX_LAYOUT; }
    if (m_entVBO) { m_rhi->DestroyBuffer(m_entVBO); m_entVBO = RHI_NULL_BUFFER; }
    m_entVertexCount = 0;
}

void SceneRenderer::DestroyRopeMeshes()
{
    if (m_ropeVL)  { m_rhi->DestroyVertexLayout(m_ropeVL); m_ropeVL = RHI_NULL_VERTEX_LAYOUT; }
    if (m_ropeVBO) { m_rhi->DestroyBuffer(m_ropeVBO); m_ropeVBO = RHI_NULL_BUFFER; }
    m_ropeVertexCount = 0;
    m_ropeDrawBatches.clear();
}

// ---- Wireframe helpers (unchanged logic) ----

static void PushBoxEdge(std::vector<float>& verts, const Vec3& a, const Vec3& b,
                         float r, float g, float bCol)
{
    verts.push_back(a.x); verts.push_back(a.y); verts.push_back(a.z);
    verts.push_back(r);   verts.push_back(g);   verts.push_back(bCol);
    verts.push_back(b.x); verts.push_back(b.y); verts.push_back(b.z);
    verts.push_back(r);   verts.push_back(g);   verts.push_back(bCol);
}

static void PushWireBox(std::vector<float>& verts, const Vec3& origin,
                         const Vec3& mins, const Vec3& maxs,
                         float r, float g, float b)
{
    Vec3 c[8];
    for (int i = 0; i < 8; i++)
    {
        c[i].x = origin.x + ((i & 1) ? maxs.x : mins.x);
        c[i].y = origin.y + ((i & 2) ? maxs.y : mins.y);
        c[i].z = origin.z + ((i & 4) ? maxs.z : mins.z);
    }
    int edges[][2] = {
        {0,1},{2,3},{4,5},{6,7},
        {0,2},{1,3},{4,6},{5,7},
        {0,4},{1,5},{2,6},{3,7},
    };
    for (auto& e : edges)
        PushBoxEdge(verts, c[e[0]], c[e[1]], r, g, b);
}

static Vec3 RotateVec3(const Mat4& m, const Vec3& v)
{
    return {
        m.m[0]*v.x + m.m[4]*v.y + m.m[8]*v.z,
        m.m[1]*v.x + m.m[5]*v.y + m.m[9]*v.z,
        m.m[2]*v.x + m.m[6]*v.y + m.m[10]*v.z
    };
}

static Mat4 EntityRotationMatrix(const VmfEntity& ent)
{
    Vec3 angles = {0, 0, 0};
    const char* str = ent.GetValue("angles", nullptr);
    if (str) sscanf(str, "%f %f %f", &angles.x, &angles.y, &angles.z);
    float pitch = angles.x * (float)(M_PI / 180.0);
    float yaw   = angles.y * (float)(M_PI / 180.0);
    float roll  = angles.z * (float)(M_PI / 180.0);
    return Mat4::RotationY(yaw) * Mat4::RotationZ(-pitch) * Mat4::RotationX(roll);
}

static void PushWireBoxRotated(std::vector<float>& verts, const Vec3& origin,
                               const Vec3& mins, const Vec3& maxs,
                               const Mat4& rotation, float r, float g, float b)
{
    Vec3 c[8];
    for (int i = 0; i < 8; i++)
    {
        Vec3 local = { (i & 1) ? maxs.x : mins.x,
                       (i & 2) ? maxs.y : mins.y,
                       (i & 4) ? maxs.z : mins.z };
        Vec3 rotated = RotateVec3(rotation, local);
        c[i] = origin + rotated;
    }
    int edges[][2] = {
        {0,1},{2,3},{4,5},{6,7},
        {0,2},{1,3},{4,6},{5,7},
        {0,4},{1,5},{2,6},{3,7},
    };
    for (auto& e : edges)
        PushBoxEdge(verts, c[e[0]], c[e[1]], r, g, b);
}

// ---- Mesh rebuilding ----

void SceneRenderer::RebuildEntityMeshes()
{
    DestroyEntityMeshes();
    if (!m_doc) return;

    std::vector<float> verts;
    float defaultSize = 8.0f;
    Vec3 defaultMins = { -defaultSize, -defaultSize, -defaultSize };
    Vec3 defaultMaxs = {  defaultSize,  defaultSize,  defaultSize };

    for (const auto& ent : m_doc->GetEntities())
    {
        if (!ent.solids.empty()) continue;

        Vec3 srcOrigin = ent.GetOrigin();
        Vec3 origin = SourceToGL(srcOrigin.x, srcOrigin.y, srcOrigin.z);
        Vec3 mins = defaultMins, maxs = defaultMaxs;
        float r = 1.0f, g = 0.0f, b = 1.0f;

        if (m_fgd && m_fgd->IsLoaded())
        {
            const openstrike::SourceFgdEntityClass* cls = m_fgd->FindClass(ent.classname.c_str());
            if (cls && cls->has_color)
            {
                r = cls->color.r / 255.0f;
                g = cls->color.g / 255.0f;
                b = cls->color.b / 255.0f;
            }
        }

        const char* modelPath = ent.GetValue("model", nullptr);
        if (modelPath && modelPath[0] != '\0' && modelPath[0] != '*')
        {
            const CachedModel& model = m_modelCache.Get(modelPath);
            if (model.valid)
            {
                float scale = 1.0f;
                const char* scaleStr = ent.GetValue("modelscale", nullptr);
                if (scaleStr) scale = (float)atof(scaleStr);
                if (scale <= 0.0f) scale = 1.0f;
                mins = model.mins * scale;
                maxs = model.maxs * scale;
            }
        }

        Mat4 rotation = EntityRotationMatrix(ent);
        PushWireBoxRotated(verts, origin, mins, maxs, rotation, r, g, b);
    }

    if (verts.empty()) return;

    m_entVertexCount = (int)(verts.size() / 6);
    m_entVBO = m_rhi->CreateBuffer(RhiBufferUsage::Vertex,
                                   verts.data(), verts.size() * sizeof(float));
    std::vector<RhiVertexAttrib> attribs = {
        { 0, 3, 6 * (int)sizeof(float), 0 },
        { 1, 3, 6 * (int)sizeof(float), 3 * (int)sizeof(float) },
    };
    m_entVL = m_rhi->CreateVertexLayout(m_entVBO, attribs);

    Log("Entity boxes: %d vertices (%d entities)",
        m_entVertexCount, (int)m_doc->GetEntities().size());
}

void SceneRenderer::RebuildRopeMeshes()
{
    DestroyRopeMeshes();
    if (!m_doc) return;

    struct RopeVert { float px, py, pz, nx, ny, nz, u, v; };
    std::vector<RopeVert> allVerts;

    struct RopeSegGroup
    {
        std::string material;
        std::vector<RopeVert> verts;
    };
    std::vector<RopeSegGroup> groups;

    std::unordered_map<std::string, const VmfEntity*> ropeByName;
    for (const auto& ent : m_doc->GetEntities())
    {
        if (ent.classname != "move_rope" && ent.classname != "keyframe_rope") continue;
        const char* tn = ent.GetValue("targetname");
        if (tn[0] != '\0') ropeByName[tn] = &ent;
    }

    for (const auto& ent : m_doc->GetEntities())
    {
        if (ent.classname != "move_rope") continue;

        std::vector<Vec3> points;
        std::string material = ent.GetValue("RopeMaterial", "cable/cable");
        float width = (float)atof(ent.GetValue("Width", "2"));
        if (width < 0.5f) width = 0.5f;
        float textureScale = (float)atof(ent.GetValue("TextureScale", "1"));
        if (textureScale <= 0.0f) textureScale = 1.0f;

        const VmfEntity* cur = &ent;
        std::unordered_set<const VmfEntity*> visited;
        { Vec3 src = cur->GetOrigin(); points.push_back(SourceToGL(src.x, src.y, src.z)); }

        while (cur)
        {
            visited.insert(cur);
            const char* nextKey = cur->GetValue("NextKey");
            if (nextKey[0] == '\0') break;
            auto it = ropeByName.find(nextKey);
            if (it == ropeByName.end()) break;
            const VmfEntity* next = it->second;
            if (visited.count(next)) break;

            Vec3 srcA = cur->GetOrigin(), srcB = next->GetOrigin();
            Vec3 a = SourceToGL(srcA.x, srcA.y, srcA.z);
            Vec3 b = SourceToGL(srcB.x, srcB.y, srcB.z);
            float slack = (float)atof(cur->GetValue("Slack", "25"));
            const int numSegs = 16;
            for (int i = 1; i <= numSegs; i++)
            {
                float t = (float)i / (float)numSegs;
                Vec3 p;
                p.x = a.x + (b.x - a.x) * t;
                p.y = a.y + (b.y - a.y) * t;
                p.z = a.z + (b.z - a.z) * t;
                p.y -= slack * 4.0f * t * (1.0f - t) * (1.0f / 64.0f);
                points.push_back(p);
            }
            cur = next;
        }

        if (points.size() < 2) continue;

        float halfW = width * 0.5f;
        Vec3 up = { 0.0f, 1.0f, 0.0f };
        const CachedTexture& tex = m_textureCache.Get(material);
        float texH = (float)std::max(tex.height, 1);
        RopeSegGroup group; group.material = material;
        float vAccum = 0.0f;

        struct LRVert { Vec3 left, right; Vec3 normal; float v; };
        std::vector<LRVert> lr; lr.reserve(points.size());
        for (size_t i = 0; i < points.size(); i++)
        {
            Vec3 tangent;
            if (i == 0) tangent = Normalize(points[1] - points[0]);
            else if (i == points.size() - 1) tangent = Normalize(points[i] - points[i - 1]);
            else tangent = Normalize(points[i + 1] - points[i - 1]);
            Vec3 right = Normalize(Cross(tangent, up));
            if (Length(right) < 0.001f) right = Normalize(Cross(tangent, Vec3{ 1.0f, 0.0f, 0.0f }));
            Vec3 normal = Normalize(Cross(right, tangent));
            if (i > 0) vAccum += Length(points[i] - points[i - 1]);
            float vCoord = (vAccum * textureScale) / texH;
            lr.push_back({ points[i] - right * halfW, points[i] + right * halfW, normal, vCoord });
        }

        for (size_t i = 0; i + 1 < lr.size(); i++)
        {
            const auto& c = lr[i]; const auto& n = lr[i + 1];
            group.verts.push_back({ c.left.x, c.left.y, c.left.z, c.normal.x, c.normal.y, c.normal.z, 0.0f, c.v });
            group.verts.push_back({ c.right.x, c.right.y, c.right.z, c.normal.x, c.normal.y, c.normal.z, 1.0f, c.v });
            group.verts.push_back({ n.right.x, n.right.y, n.right.z, n.normal.x, n.normal.y, n.normal.z, 1.0f, n.v });
            group.verts.push_back({ c.left.x, c.left.y, c.left.z, c.normal.x, c.normal.y, c.normal.z, 0.0f, c.v });
            group.verts.push_back({ n.right.x, n.right.y, n.right.z, n.normal.x, n.normal.y, n.normal.z, 1.0f, n.v });
            group.verts.push_back({ n.left.x, n.left.y, n.left.z, n.normal.x, n.normal.y, n.normal.z, 0.0f, n.v });
        }
        if (!group.verts.empty()) groups.push_back(std::move(group));
    }

    if (groups.empty()) return;

    std::sort(groups.begin(), groups.end(),
        [](const RopeSegGroup& a, const RopeSegGroup& b) { return a.material < b.material; });

    std::string currentMat; int batchStart = 0;
    for (const auto& g : groups)
    {
        if (g.material != currentMat)
        {
            int count = (int)allVerts.size() - batchStart;
            if (count > 0)
            {
                const CachedTexture& prevTex = m_textureCache.Get(currentMat);
                m_ropeDrawBatches.push_back({ prevTex.rhiTexture, batchStart, count });
            }
            currentMat = g.material; batchStart = (int)allVerts.size();
        }
        allVerts.insert(allVerts.end(), g.verts.begin(), g.verts.end());
    }
    { int count = (int)allVerts.size() - batchStart;
      if (count > 0) {
          const CachedTexture& tex = m_textureCache.Get(currentMat);
          m_ropeDrawBatches.push_back({ tex.rhiTexture, batchStart, count });
      }
    }

    if (allVerts.empty()) return;
    m_ropeVertexCount = (int)allVerts.size();

    m_ropeVBO = m_rhi->CreateBuffer(RhiBufferUsage::Vertex,
                                    allVerts.data(), allVerts.size() * sizeof(RopeVert));
    std::vector<RhiVertexAttrib> attribs = {
        { 0, 3, (int)sizeof(RopeVert), 0 },
        { 1, 3, (int)sizeof(RopeVert), 3 * (int)sizeof(float) },
        { 2, 2, (int)sizeof(RopeVert), 6 * (int)sizeof(float) },
    };
    m_ropeVL = m_rhi->CreateVertexLayout(m_ropeVBO, attribs);

    Log("Rope meshes: %d vertices, %d batches", m_ropeVertexCount, (int)m_ropeDrawBatches.size());
}

void SceneRenderer::RebuildModelInstances()
{
    m_modelInstances.clear();
    if (!m_doc) return;

    m_modelCache.SetLogFunc(m_logFunc);
    int totalWithModel = 0, totalFailed = 0;

    for (const auto& ent : m_doc->GetEntities())
    {
        if (!ent.solids.empty()) continue;
        const char* modelPath = ent.GetValue("model", nullptr);
        if (!modelPath || modelPath[0] == '\0' || modelPath[0] == '*') continue;
        totalWithModel++;
        const CachedModel& model = m_modelCache.Get(modelPath);
        if (!model.valid) { totalFailed++; continue; }

        Vec3 srcOrigin = ent.GetOrigin();
        Vec3 origin = SourceToGL(srcOrigin.x, srcOrigin.y, srcOrigin.z);
        Vec3 angles = {0, 0, 0};
        const char* anglesStr = ent.GetValue("angles", nullptr);
        if (anglesStr) sscanf(anglesStr, "%f %f %f", &angles.x, &angles.y, &angles.z);
        float scale = 1.0f;
        const char* scaleStr = ent.GetValue("modelscale", nullptr);
        if (scaleStr) scale = (float)atof(scaleStr);
        if (scale <= 0.0f) scale = 1.0f;
        m_modelInstances.push_back({ &model, origin, angles, scale });
    }
    Log("Model instances: %d (%d entities with models, %d failed to load)",
        (int)m_modelInstances.size(), totalWithModel, totalFailed);
}

void SceneRenderer::RebuildBrushMeshes()
{
    DestroyBrushMeshes();
    m_drawBatches.clear();
    if (!m_doc) return;

    m_textureCache.SetLogFunc(m_logFunc);

    struct SolidVert { float px, py, pz, nx, ny, nz, u, v, pickId; };
    struct WireVert { float px, py, pz, nx, ny, nz; };
    std::vector<WireVert> bspWireVerts, dispWireVerts;

    int totalSolids = 0, totalFaces = 0, emptySolids = 0;

    struct FaceEntry { BrushFace face; float pickId; };
    std::vector<FaceEntry> allFaces;
    m_worldSolidCount = (int)m_doc->GetWorld().solids.size();

    auto processSolids = [&](const std::vector<VmfSolid>& solids, int pickIdBase) {
        for (int si = 0; si < (int)solids.size(); si++)
        {
            const auto& solid = solids[si];
            float pickId = (float)(pickIdBase + si);
            totalSolids++;
            BrushMesh bm = BuildBrushMesh(solid);
            if (bm.faces.empty()) { emptySolids++; if (emptySolids <= 3) Log("  Empty solid #%d: %d sides", solid.id, (int)solid.sides.size()); }
            for (auto& face : bm.faces)
            {
                if (face.vertices.size() < 3) continue;
                if ((!m_toolVis.showSkip       && IsToolTexture(face.material, "skip")) ||
                    (!m_toolVis.showHint        && IsToolTexture(face.material, "hint")) ||
                    (!m_toolVis.showNodraw       && IsToolTexture(face.material, "nodraw")) ||
                    (!m_toolVis.showPlayerClip   && IsToolTexture(face.material, "playerclip")) ||
                    (!m_toolVis.showAreaPortal   && IsToolTexture(face.material, "areaportal")) ||
                    (!m_toolVis.showLadder       && IsToolTexture(face.material, "ladder")) ||
                    (!m_toolVis.showTrigger      && IsToolTexture(face.material, "trigger")))
                    continue;
                totalFaces++;
                auto& wireTarget = face.isDisplacement ? dispWireVerts : bspWireVerts;
                for (size_t i = 0; i < face.vertices.size(); i++)
                {
                    const auto& a = face.vertices[i];
                    const auto& bv = face.vertices[(i + 1) % face.vertices.size()];
                    wireTarget.push_back({ a.pos.x, a.pos.y, a.pos.z, a.normal.x, a.normal.y, a.normal.z });
                    wireTarget.push_back({ bv.pos.x, bv.pos.y, bv.pos.z, bv.normal.x, bv.normal.y, bv.normal.z });
                }
                allFaces.push_back({ std::move(face), pickId });
            }
        }
    };

    processSolids(m_doc->GetWorld().solids, 1);
    int entPickBase = m_worldSolidCount + 1;
    for (const auto& ent : m_doc->GetEntities())
    {
        processSolids(ent.solids, entPickBase);
        entPickBase += (int)ent.solids.size();
    }

    std::sort(allFaces.begin(), allFaces.end(),
        [](const FaceEntry& a, const FaceEntry& b) { return a.face.material < b.face.material; });

    std::vector<SolidVert> triVerts;
    std::string currentMat; int batchStart = 0;
    for (const auto& entry : allFaces)
    {
        const auto& face = entry.face;
        const CachedTexture& tex = m_textureCache.Get(face.material);
        float texW = (float)std::max(tex.width, 1);
        float texH = (float)std::max(tex.height, 1);
        if (face.material != currentMat)
        {
            int count = (int)triVerts.size() - batchStart;
            if (count > 0)
            {
                const CachedTexture& prevTex = m_textureCache.Get(currentMat);
                m_drawBatches.push_back({ prevTex.rhiTexture, batchStart, count });
            }
            currentMat = face.material; batchStart = (int)triVerts.size();
        }
        float pid = entry.pickId;
        for (size_t i = 1; i + 1 < face.vertices.size(); i++)
        {
            auto push = [&](const BrushVertex& v) {
                triVerts.push_back({ v.pos.x, v.pos.y, v.pos.z,
                                     v.normal.x, v.normal.y, v.normal.z,
                                     v.u / texW, v.v / texH, pid });
            };
            push(face.vertices[0]); push(face.vertices[i]); push(face.vertices[i + 1]);
        }
    }
    { int count = (int)triVerts.size() - batchStart;
      if (count > 0) {
          const CachedTexture& tex = m_textureCache.Get(currentMat);
          m_drawBatches.push_back({ tex.rhiTexture, batchStart, count });
      }
    }

    Log("Brush rebuild: %d solids, %d faces, %d tri verts, %d bsp wire / %d disp wire verts, %d batches",
        totalSolids, totalFaces, (int)triVerts.size(),
        (int)bspWireVerts.size(), (int)dispWireVerts.size(), (int)m_drawBatches.size());
    if (emptySolids > 0) Log("  %d/%d solids produced no geometry", emptySolids, totalSolids);
    if (triVerts.empty()) Log("WARNING: No brush geometry generated!");

    // Upload solid triangles (9 floats per vertex: pos + normal + uv + pickId)
    if (!triVerts.empty())
    {
        m_brushVertexCount = (int)triVerts.size();
        m_brushVBO = m_rhi->CreateBuffer(RhiBufferUsage::Vertex,
                                         triVerts.data(), triVerts.size() * sizeof(SolidVert));
        std::vector<RhiVertexAttrib> attribs = {
            { 0, 3, (int)sizeof(SolidVert), 0 },
            { 1, 3, (int)sizeof(SolidVert), 3 * (int)sizeof(float) },
            { 2, 2, (int)sizeof(SolidVert), 6 * (int)sizeof(float) },
            { 3, 1, (int)sizeof(SolidVert), 8 * (int)sizeof(float) },
        };
        m_brushVL = m_rhi->CreateVertexLayout(m_brushVBO, attribs);
    }

    // Upload wireframe
    auto uploadWire = [&](const std::vector<WireVert>& wv, RhiVertexLayout& vl, RhiBuffer& vbo, int& count) {
        if (wv.empty()) return;
        count = (int)wv.size();
        vbo = m_rhi->CreateBuffer(RhiBufferUsage::Vertex, wv.data(), wv.size() * sizeof(WireVert));
        std::vector<RhiVertexAttrib> attribs = {
            { 0, 3, (int)sizeof(WireVert), 0 },
            { 1, 3, (int)sizeof(WireVert), 3 * (int)sizeof(float) },
        };
        vl = m_rhi->CreateVertexLayout(vbo, attribs);
    };
    uploadWire(bspWireVerts, m_bspWireVL, m_bspWireVBO, m_bspWireVertexCount);
    uploadWire(dispWireVerts, m_dispWireVL, m_dispWireVBO, m_dispWireVertexCount);
}

// ---- Pick ID helper ----

static void EncodePickId(int id, float& r, float& g, float& b, float& a)
{
    r = (float)(id & 0xFF) / 255.0f;
    g = (float)((id >> 8) & 0xFF) / 255.0f;
    b = 0.0f;
    a = 1.0f;
}

// ---- Render ----

void SceneRenderer::Render(const Mat4& view, const Mat4& proj)
{
    if (m_dirty)
    {
        m_dirty = false;
        RebuildBrushMeshes();
        RebuildEntityMeshes();
        RebuildRopeMeshes();
        RebuildModelInstances();
    }

    Mat4 vp = proj * view;

    m_rhi->BindShader(m_sceneShader);
    m_rhi->SetUniformMat4(m_locMVP, vp.Ptr());
    m_rhi->SetDepthTest(true);
    m_rhi->SetCullFace(false);
    m_rhi->SetUniformInt(m_locUseVertexColor, 0);
    m_rhi->SetUniformInt(m_locUseTexture, 0);

    // Grid
    m_rhi->SetUniformVec4(m_locColor, 0.3f, 0.3f, 0.3f, 1.0f);
    m_rhi->SetUniformInt(m_locUseLighting, 0);
    m_rhi->BindVertexLayout(m_gridVL);
    m_rhi->Draw(RhiPrimitive::Lines, 0, m_gridVertexCount);

    // Brush solids
    if (m_brushVertexCount > 0 && !m_drawBatches.empty())
    {
        m_rhi->SetPolygonOffsetFill(true);
        m_rhi->SetUniformInt(m_locTexSampler, 0);
        m_rhi->SetUniformInt(m_locUseTexture, 1);
        m_rhi->SetUniformVec4(m_locColor, 1.0f, 1.0f, 1.0f, 1.0f);
        m_rhi->SetUniformInt(m_locUseLighting, 1);
        m_rhi->BindVertexLayout(m_brushVL);

        for (const auto& batch : m_drawBatches)
        {
            m_rhi->BindTexture(batch.rhiTexture, 0);
            m_rhi->Draw(RhiPrimitive::Triangles, batch.startVertex, batch.vertexCount);
        }
        m_rhi->SetUniformInt(m_locUseTexture, 0);
        m_rhi->UnbindTexture(0);
        m_rhi->SetPolygonOffsetFill(false);
    }

    // Models
    RenderModels(vp);
    RenderModelPreview(vp);

    // Reset MVP for wireframe/entities
    m_rhi->SetUniformMat4(m_locMVP, vp.Ptr());

    // BSP wireframe
    if (m_showBspWireframe && m_bspWireVertexCount > 0)
    {
        m_rhi->SetUniformVec4(m_locColor, 1.0f, 1.0f, 1.0f, 1.0f);
        m_rhi->SetUniformInt(m_locUseLighting, 0);
        m_rhi->BindVertexLayout(m_bspWireVL);
        m_rhi->Draw(RhiPrimitive::Lines, 0, m_bspWireVertexCount);
    }

    // Displacement wireframe
    if (m_showDispWireframe && m_dispWireVertexCount > 0)
    {
        m_rhi->SetUniformVec4(m_locColor, 1.0f, 1.0f, 1.0f, 1.0f);
        m_rhi->SetUniformInt(m_locUseLighting, 0);
        m_rhi->BindVertexLayout(m_dispWireVL);
        m_rhi->Draw(RhiPrimitive::Lines, 0, m_dispWireVertexCount);
    }

    // Entity boxes
    if (m_entVertexCount > 0)
    {
        m_rhi->SetUniformInt(m_locUseLighting, 0);
        m_rhi->SetUniformInt(m_locUseVertexColor, 1);
        m_rhi->BindVertexLayout(m_entVL);
        m_rhi->Draw(RhiPrimitive::Lines, 0, m_entVertexCount);
        m_rhi->SetUniformInt(m_locUseVertexColor, 0);
    }

    // Textured ropes
    if (m_ropeVertexCount > 0 && !m_ropeDrawBatches.empty())
    {
        m_rhi->SetUniformInt(m_locTexSampler, 0);
        m_rhi->SetUniformInt(m_locUseTexture, 1);
        m_rhi->SetUniformVec4(m_locColor, 1.0f, 1.0f, 1.0f, 1.0f);
        m_rhi->SetUniformInt(m_locUseLighting, 1);
        m_rhi->BindVertexLayout(m_ropeVL);

        for (const auto& batch : m_ropeDrawBatches)
        {
            m_rhi->BindTexture(batch.rhiTexture, 0);
            m_rhi->Draw(RhiPrimitive::Triangles, batch.startVertex, batch.vertexCount);
        }
        m_rhi->SetUniformInt(m_locUseTexture, 0);
        m_rhi->UnbindTexture(0);
    }

    // Leak trail
    if (m_showLeakTrail && m_leakVertexCount > 0)
    {
        m_rhi->SetDepthTest(false);
        m_rhi->SetUniformInt(m_locUseLighting, 0);
        m_rhi->SetUniformInt(m_locUseVertexColor, 1);
        m_rhi->BindVertexLayout(m_leakVL);
        m_rhi->SetLineWidth(3.0f);
        m_rhi->Draw(RhiPrimitive::Lines, 0, m_leakVertexCount);
        m_rhi->SetLineWidth(1.0f);
        m_rhi->SetUniformInt(m_locUseVertexColor, 0);
        m_rhi->SetDepthTest(true);
    }

    // Brush creation preview
    if (m_showPreview)
    {
        std::vector<float> verts;
        PushWireBox(verts, {0, 0, 0}, m_previewMins, m_previewMaxs, 0.2f, 1.0f, 0.2f);
        DrawTempWireframe(verts, 2.0f);
    }

    // Resize handles
    if (m_showResizeHandles)
    {
        std::vector<float> verts;
        Vec3 centers[6]; GetResizeHandleCenters(centers);
        float handleSize = 4.0f;
        for (int i = 0; i < 6; i++)
        {
            Vec3 hmins = { -handleSize, -handleSize, -handleSize };
            Vec3 hmaxs = {  handleSize,  handleSize,  handleSize };
            PushWireBox(verts, centers[i], hmins, hmaxs, 1.0f, 1.0f, 0.0f);
        }
        DrawTempWireframe(verts, 2.0f);
    }

    // Clip preview
    if (m_showClipPreview)
    {
        std::vector<float> verts;
        Vec3 frontMins = m_clipBrushMinsGL, frontMaxs = m_clipBrushMaxsGL;
        Vec3 backMins = m_clipBrushMinsGL, backMaxs = m_clipBrushMaxsGL;
        (&frontMins.x)[m_clipGLAxis] = m_clipGLAxisPos;
        (&backMaxs.x)[m_clipGLAxis] = m_clipGLAxisPos;

        float fr, fg, fb, br, bg, bb;
        if (m_clipMode == 0) { fr=0.2f; fg=1.0f; fb=0.2f; br=0.2f; bg=0.5f; bb=1.0f; }
        else if (m_clipMode == 1) { fr=0.2f; fg=1.0f; fb=0.2f; br=1.0f; bg=0.2f; bb=0.2f; }
        else { fr=1.0f; fg=0.2f; fb=0.2f; br=0.2f; bg=1.0f; bb=0.2f; }

        PushWireBox(verts, {0,0,0}, frontMins, frontMaxs, fr, fg, fb);
        PushWireBox(verts, {0,0,0}, backMins, backMaxs, br, bg, bb);

        // Clip plane
        Vec3 p[4];
        if (m_clipGLAxis == 0) {
            p[0]={m_clipGLAxisPos,m_clipBrushMinsGL.y,m_clipBrushMinsGL.z};
            p[1]={m_clipGLAxisPos,m_clipBrushMaxsGL.y,m_clipBrushMinsGL.z};
            p[2]={m_clipGLAxisPos,m_clipBrushMaxsGL.y,m_clipBrushMaxsGL.z};
            p[3]={m_clipGLAxisPos,m_clipBrushMinsGL.y,m_clipBrushMaxsGL.z};
        } else if (m_clipGLAxis == 1) {
            p[0]={m_clipBrushMinsGL.x,m_clipGLAxisPos,m_clipBrushMinsGL.z};
            p[1]={m_clipBrushMaxsGL.x,m_clipGLAxisPos,m_clipBrushMinsGL.z};
            p[2]={m_clipBrushMaxsGL.x,m_clipGLAxisPos,m_clipBrushMaxsGL.z};
            p[3]={m_clipBrushMinsGL.x,m_clipGLAxisPos,m_clipBrushMaxsGL.z};
        } else {
            p[0]={m_clipBrushMinsGL.x,m_clipBrushMinsGL.y,m_clipGLAxisPos};
            p[1]={m_clipBrushMaxsGL.x,m_clipBrushMinsGL.y,m_clipGLAxisPos};
            p[2]={m_clipBrushMaxsGL.x,m_clipBrushMaxsGL.y,m_clipGLAxisPos};
            p[3]={m_clipBrushMinsGL.x,m_clipBrushMaxsGL.y,m_clipGLAxisPos};
        }
        PushBoxEdge(verts, p[0], p[1], 1.0f, 1.0f, 0.0f);
        PushBoxEdge(verts, p[1], p[2], 1.0f, 1.0f, 0.0f);
        PushBoxEdge(verts, p[2], p[3], 1.0f, 1.0f, 0.0f);
        PushBoxEdge(verts, p[3], p[0], 1.0f, 1.0f, 0.0f);
        DrawTempWireframe(verts, 2.0f);
    }

    // Displacement cursor
    if (m_showDispCursor)
    {
        std::vector<float> verts;
        Vec3 n = Normalize(m_dispCursorNormal);
        Vec3 ref = { 0, 0, 1 };
        if (fabsf(Dot(n, ref)) > 0.9f) ref = { 1, 0, 0 };
        Vec3 tangent1 = Normalize(Cross(n, ref));
        Vec3 tangent2 = Cross(n, tangent1);
        float r = m_dispCursorRadius;
        const int segments = 32;
        for (int i = 0; i < segments; i++)
        {
            float a0 = (float)i / (float)segments * 2.0f * (float)M_PI;
            float a1 = (float)(i + 1) / (float)segments * 2.0f * (float)M_PI;
            Vec3 p0 = m_dispCursorCenter + tangent1 * (cosf(a0) * r) + tangent2 * (sinf(a0) * r);
            Vec3 p1 = m_dispCursorCenter + tangent1 * (cosf(a1) * r) + tangent2 * (sinf(a1) * r);
            PushBoxEdge(verts, p0, p1, 0.0f, 1.0f, 1.0f);
        }
        DrawTempWireframe(verts, 2.0f);
    }

    // Selection highlight
    RenderSelectionHighlight(vp);

    m_rhi->BindVertexLayout(RHI_NULL_VERTEX_LAYOUT);
    m_rhi->BindShader(RHI_NULL_SHADER);
}

void SceneRenderer::RenderPick(const Mat4& view, const Mat4& proj)
{
    if (m_dirty)
    {
        m_dirty = false;
        RebuildBrushMeshes();
        RebuildEntityMeshes();
        RebuildRopeMeshes();
        RebuildModelInstances();
    }

    Mat4 vp = proj * view;

    m_rhi->SetDepthTest(true);
    m_rhi->SetCullFace(false);
    m_rhi->SetBlend(false);

    // Brushes with per-vertex pick IDs
    if (m_brushVertexCount > 0 && m_pickShader)
    {
        m_rhi->BindShader(m_pickShader);
        m_rhi->SetUniformMat4(m_locPickMVP, vp.Ptr());
        m_rhi->BindVertexLayout(m_brushVL);
        m_rhi->Draw(RhiPrimitive::Triangles, 0, m_brushVertexCount);
    }

    // Entities as filled cubes with per-entity pick IDs
    if (m_doc && m_cubeVL)
    {
        m_rhi->BindShader(m_sceneShader);
        m_rhi->SetUniformInt(m_locUseLighting, 0);
        m_rhi->SetUniformInt(m_locUseVertexColor, 0);
        m_rhi->SetUniformInt(m_locUseTexture, 0);

        float defaultSize = 8.0f;
        const auto& entities = m_doc->GetEntities();
        int entPickBase = m_worldSolidCount + 1;

        for (int i = 0; i < (int)entities.size(); i++)
        {
            const auto& ent = entities[i];
            if (!ent.solids.empty()) continue;

            int pickId = entPickBase + i;
            Vec3 srcOrigin = ent.GetOrigin();
            Vec3 origin = SourceToGL(srcOrigin.x, srcOrigin.y, srcOrigin.z);

            Vec3 mins = { -defaultSize, -defaultSize, -defaultSize };
            Vec3 maxs = {  defaultSize,  defaultSize,  defaultSize };

            const char* modelPath = ent.GetValue("model", nullptr);
            if (modelPath && modelPath[0] != '\0' && modelPath[0] != '*')
            {
                const CachedModel* cachedModel = m_modelCache.Find(modelPath);
                if (cachedModel)
                {
                    float scale = 1.0f;
                    const char* scaleStr = ent.GetValue("modelscale", nullptr);
                    if (scaleStr) scale = (float)atof(scaleStr);
                    if (scale <= 0.0f) scale = 1.0f;
                    mins = cachedModel->mins * scale;
                    maxs = cachedModel->maxs * scale;
                }
            }

            Mat4 rotation = EntityRotationMatrix(ent);
            Vec3 localCenter = { (mins.x+maxs.x)*0.5f, (mins.y+maxs.y)*0.5f, (mins.z+maxs.z)*0.5f };
            Vec3 halfExt = { (maxs.x-mins.x)*0.5f, (maxs.y-mins.y)*0.5f, (maxs.z-mins.z)*0.5f };
            Mat4 modelMat = Mat4::Translation(origin) * rotation
                          * Mat4::Translation(localCenter)
                          * Mat4::Scale(halfExt.x, halfExt.y, halfExt.z);
            Mat4 mvp = vp * modelMat;

            m_rhi->SetUniformMat4(m_locMVP, mvp.Ptr());
            float r, g, b, a; EncodePickId(pickId, r, g, b, a);
            m_rhi->SetUniformVec4(m_locColor, r, g, b, a);

            m_rhi->BindVertexLayout(m_cubeVL);
            m_rhi->Draw(RhiPrimitive::Triangles, 0, 36);
        }

        // Model meshes with pick IDs
        for (int i = 0; i < (int)entities.size(); i++)
        {
            const auto& ent = entities[i];
            if (!ent.solids.empty()) continue;
            int pickId = entPickBase + i;
            const char* modelPath = ent.GetValue("model", nullptr);
            if (!modelPath || modelPath[0] == '\0' || modelPath[0] == '*') continue;
            const CachedModel* cachedModel = m_modelCache.Find(modelPath);
            if (!cachedModel) continue;

            Vec3 srcOrigin = ent.GetOrigin();
            Vec3 origin = SourceToGL(srcOrigin.x, srcOrigin.y, srcOrigin.z);
            float scale = 1.0f;
            const char* scaleStr = ent.GetValue("modelscale", nullptr);
            if (scaleStr) scale = (float)atof(scaleStr);
            if (scale <= 0.0f) scale = 1.0f;

            Vec3 angles = {0,0,0};
            const char* anglesStr = ent.GetValue("angles", nullptr);
            if (anglesStr) sscanf(anglesStr, "%f %f %f", &angles.x, &angles.y, &angles.z);
            float pitch = angles.x * (float)(M_PI / 180.0);
            float yaw   = angles.y * (float)(M_PI / 180.0);
            float roll  = angles.z * (float)(M_PI / 180.0);
            Mat4 rotation = Mat4::RotationY(yaw) * Mat4::RotationZ(-pitch) * Mat4::RotationX(roll);
            Mat4 mvp = vp * Mat4::Translation(origin) * rotation * Mat4::Scale(scale);

            m_rhi->SetUniformMat4(m_locMVP, mvp.Ptr());
            float r, g, b, a; EncodePickId(pickId, r, g, b, a);
            m_rhi->SetUniformVec4(m_locColor, r, g, b, a);

            m_rhi->BindVertexLayout(cachedModel->vl);
            m_rhi->DrawIndexed(RhiPrimitive::Triangles, cachedModel->indexCount);
        }

        m_rhi->BindVertexLayout(RHI_NULL_VERTEX_LAYOUT);
    }

    m_rhi->BindShader(RHI_NULL_SHADER);
}

void SceneRenderer::RenderModels(const Mat4& vp)
{
    if (m_modelInstances.empty()) return;

    m_rhi->SetUniformInt(m_locUseLighting, 1);
    m_rhi->SetUniformVec4(m_locColor, 1.0f, 1.0f, 1.0f, 1.0f);

    for (const auto& inst : m_modelInstances)
    {
        float pitch = inst.angles.x * (float)(M_PI / 180.0);
        float yaw   = inst.angles.y * (float)(M_PI / 180.0);
        float roll  = inst.angles.z * (float)(M_PI / 180.0);
        Mat4 rotation = Mat4::RotationY(yaw) * Mat4::RotationZ(-pitch) * Mat4::RotationX(roll);
        Mat4 modelMat = Mat4::Translation(inst.position) * rotation * Mat4::Scale(inst.scale);
        Mat4 mvp = vp * modelMat;
        m_rhi->SetUniformMat4(m_locMVP, mvp.Ptr());

        if (!inst.model->materialName.empty())
        {
            const CachedTexture& tex = m_textureCache.Get(inst.model->materialName);
            m_rhi->BindTexture(tex.rhiTexture, 0);
            m_rhi->SetUniformInt(m_locUseTexture, 1);
        }
        else
        {
            m_rhi->UnbindTexture(0);
            m_rhi->SetUniformInt(m_locUseTexture, 0);
        }

        m_rhi->BindVertexLayout(inst.model->vl);
        m_rhi->DrawIndexed(RhiPrimitive::Triangles, inst.model->indexCount);
    }

    m_rhi->BindVertexLayout(RHI_NULL_VERTEX_LAYOUT);
    m_rhi->UnbindTexture(0);
    m_rhi->SetUniformInt(m_locUseTexture, 0);
}

void SceneRenderer::RenderModelPreview(const Mat4& vp)
{
    if (!m_showModelPreview || !m_previewModel || !m_previewModel->valid) return;

    m_rhi->SetBlend(true);
    m_rhi->SetUniformInt(m_locUseLighting, 1);
    m_rhi->SetUniformVec4(m_locColor, 1.0f, 1.0f, 1.0f, 0.6f);

    float pitch = m_previewModelAngles.x * (float)(M_PI / 180.0);
    float yaw   = m_previewModelAngles.y * (float)(M_PI / 180.0);
    float roll  = m_previewModelAngles.z * (float)(M_PI / 180.0);
    Mat4 rotation = Mat4::RotationY(yaw) * Mat4::RotationZ(-pitch) * Mat4::RotationX(roll);
    Mat4 modelMat = Mat4::Translation(m_previewModelPos) * rotation * Mat4::Scale(m_previewModelScale);
    Mat4 mvp = vp * modelMat;
    m_rhi->SetUniformMat4(m_locMVP, mvp.Ptr());

    if (!m_previewModel->materialName.empty())
    {
        const CachedTexture& tex = m_textureCache.Get(m_previewModel->materialName);
        m_rhi->BindTexture(tex.rhiTexture, 0);
        m_rhi->SetUniformInt(m_locUseTexture, 1);
    }
    else
    {
        m_rhi->UnbindTexture(0);
        m_rhi->SetUniformInt(m_locUseTexture, 0);
    }

    m_rhi->BindVertexLayout(m_previewModel->vl);
    m_rhi->DrawIndexed(RhiPrimitive::Triangles, m_previewModel->indexCount);

    m_rhi->BindVertexLayout(RHI_NULL_VERTEX_LAYOUT);
    m_rhi->UnbindTexture(0);
    m_rhi->SetUniformInt(m_locUseTexture, 0);
    m_rhi->SetBlend(false);
}

void SceneRenderer::RenderSelectionHighlight(const Mat4& vp)
{
    if (!m_doc) return;

    std::vector<float> verts;
    const auto& sel = m_doc->GetSelection();

    if (sel.type == SelectionType::WorldSolid && sel.index >= 0)
    {
        const auto& solids = m_doc->GetWorld().solids;
        if (sel.index < (int)solids.size())
        {
            Vec3 mins, maxs;
            VmfDocument::ComputeSolidBoundsGL(solids[sel.index], mins, maxs);
            float pad = 1.0f;
            mins = mins - Vec3{pad,pad,pad}; maxs = maxs + Vec3{pad,pad,pad};
            PushWireBox(verts, {0,0,0}, mins, maxs, 1.0f, 0.5f, 0.0f);
        }
    }
    else if (m_selectedEntityIdx >= 0)
    {
        const auto& entities = m_doc->GetEntities();
        if (m_selectedEntityIdx < (int)entities.size())
        {
            const auto& ent = entities[m_selectedEntityIdx];
            if (ent.solids.empty())
            {
                Vec3 srcOrigin = ent.GetOrigin();
                Vec3 origin = SourceToGL(srcOrigin.x, srcOrigin.y, srcOrigin.z);
                float defaultSize = 8.0f;
                Vec3 mins = {-defaultSize,-defaultSize,-defaultSize};
                Vec3 maxs = { defaultSize, defaultSize, defaultSize};
                const char* modelPath = ent.GetValue("model", nullptr);
                if (modelPath && modelPath[0] != '\0' && modelPath[0] != '*')
                {
                    const CachedModel& model = m_modelCache.Get(modelPath);
                    if (model.valid)
                    {
                        float scale = 1.0f;
                        const char* scaleStr = ent.GetValue("modelscale", nullptr);
                        if (scaleStr) scale = (float)atof(scaleStr);
                        if (scale <= 0.0f) scale = 1.0f;
                        mins = model.mins * scale; maxs = model.maxs * scale;
                    }
                }
                float pad = 1.0f;
                mins = mins - Vec3{pad,pad,pad}; maxs = maxs + Vec3{pad,pad,pad};
                Mat4 rotation = EntityRotationMatrix(ent);
                PushWireBoxRotated(verts, origin, mins, maxs, rotation, 1.0f, 0.8f, 0.0f);
            }
        }
    }

    if (verts.empty()) return;

    m_rhi->SetUniformMat4(m_locMVP, vp.Ptr());
    DrawTempWireframe(verts, 2.0f);
}

// ---- Leak trail ----

void SceneRenderer::SetLeakTrail(const std::vector<Vec3>& pointsGL)
{
    ClearLeakTrail();
    if (pointsGL.size() < 2) return;

    std::vector<float> verts;
    verts.reserve(pointsGL.size() * 2 * 6);
    for (size_t i = 0; i + 1 < pointsGL.size(); i++)
    {
        const Vec3& a = pointsGL[i];
        const Vec3& b = pointsGL[i + 1];
        PushBoxEdge(verts, a, b, 1.0f, 0.0f, 0.0f);
    }

    m_leakVertexCount = (int)(verts.size() / 6);
    m_leakVBO = m_rhi->CreateBuffer(RhiBufferUsage::Vertex,
                                    verts.data(), verts.size() * sizeof(float));
    std::vector<RhiVertexAttrib> attribs = {
        { 0, 3, 6 * (int)sizeof(float), 0 },
        { 1, 3, 6 * (int)sizeof(float), 3 * (int)sizeof(float) },
    };
    m_leakVL = m_rhi->CreateVertexLayout(m_leakVBO, attribs);
    m_showLeakTrail = true;
}

void SceneRenderer::ClearLeakTrail()
{
    if (m_leakVL)  { m_rhi->DestroyVertexLayout(m_leakVL); m_leakVL = RHI_NULL_VERTEX_LAYOUT; }
    if (m_leakVBO) { m_rhi->DestroyBuffer(m_leakVBO); m_leakVBO = RHI_NULL_BUFFER; }
    m_leakVertexCount = 0;
    m_showLeakTrail = false;
}

// ---- State setters (unchanged) ----

void SceneRenderer::SetPreviewBox(const Vec3& mins, const Vec3& maxs)
{
    m_showPreview = true; m_previewMins = mins; m_previewMaxs = maxs;
}

void SceneRenderer::SetClipPreview(const Vec3& brushMinsGL, const Vec3& brushMaxsGL,
                                    int glAxis, float glAxisPos, int mode)
{
    m_showClipPreview = true;
    m_clipBrushMinsGL = brushMinsGL; m_clipBrushMaxsGL = brushMaxsGL;
    m_clipGLAxis = glAxis; m_clipGLAxisPos = glAxisPos; m_clipMode = mode;
}

void SceneRenderer::SetResizeHandles(const Vec3& mins, const Vec3& maxs)
{
    m_showResizeHandles = true; m_resizeMins = mins; m_resizeMaxs = maxs;
}

void SceneRenderer::GetResizeHandleCenters(Vec3 centers[6]) const
{
    float cx = (m_resizeMins.x + m_resizeMaxs.x) * 0.5f;
    float cy = (m_resizeMins.y + m_resizeMaxs.y) * 0.5f;
    float cz = (m_resizeMins.z + m_resizeMaxs.z) * 0.5f;
    centers[0] = { m_resizeMaxs.x, cy, cz };
    centers[1] = { m_resizeMins.x, cy, cz };
    centers[2] = { cx, m_resizeMaxs.y, cz };
    centers[3] = { cx, m_resizeMins.y, cz };
    centers[4] = { cx, cy, m_resizeMaxs.z };
    centers[5] = { cx, cy, m_resizeMins.z };
}

void SceneRenderer::SetDispBrushCursor(const Vec3& center, const Vec3& normal, float radius)
{
    m_showDispCursor = true;
    m_dispCursorCenter = center; m_dispCursorNormal = normal; m_dispCursorRadius = radius;
}

void SceneRenderer::SetModelPreview(const CachedModel* model, const Vec3& posGL,
                                     const Vec3& angles, float scale)
{
    m_showModelPreview = true;
    m_previewModel = model; m_previewModelPos = posGL;
    m_previewModelAngles = angles; m_previewModelScale = scale;
}
