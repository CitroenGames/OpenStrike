#pragma once

#include "ed_math.hpp"
#include "rhi/rhi_types.hpp"
#include "texture_cache.hpp"
#include "model_cache.hpp"
#include <cstdint>
#include <functional>
#include <vector>

class Rhi;
class VmfDocument;
class FgdManager;

struct ToolVisibility
{
    bool showSkip       = true;
    bool showHint       = true;
    bool showNodraw     = true;
    bool showPlayerClip = true;
    bool showAreaPortal = true;
    bool showLadder     = true;
    bool showTrigger    = true;
};

struct DrawBatch
{
    RhiTexture rhiTexture = RHI_NULL_TEXTURE;
    int        startVertex = 0;
    int        vertexCount = 0;
};

class SceneRenderer
{
public:
    bool Init(Rhi* rhi);
    void Shutdown();
    void Render(const Mat4& view, const Mat4& proj);
    void RenderPick(const Mat4& view, const Mat4& proj);

    void SetDocument(VmfDocument* doc, FgdManager* fgd = nullptr);
    void SetLogFunc(std::function<void(const char*)> fn) { m_logFunc = fn; }
    void SetGameDir(const char* dir);
    void MarkDirty() { m_dirty = true; }
    void SetSelectedEntityIndex(int idx) { m_selectedEntityIdx = idx; }
    int  GetWorldSolidCount() const { return m_worldSolidCount; }
    const ModelCache& GetModelCache() const { return m_modelCache; }
    ModelCache& GetModelCache() { return m_modelCache; }

    ToolVisibility m_toolVis;
    bool m_showBspWireframe  = true;
    bool m_showDispWireframe = true;

    // Preview box for brush creation
    void SetPreviewBox(const Vec3& mins, const Vec3& maxs);
    void HidePreview() { m_showPreview = false; }

    // Clip preview for brush clipping
    void SetClipPreview(const Vec3& brushMinsGL, const Vec3& brushMaxsGL,
                        int glAxis, float glAxisPos, int mode);
    void HideClipPreview() { m_showClipPreview = false; }

    // Resize handles for brush editing
    void SetResizeHandles(const Vec3& mins, const Vec3& maxs);
    void HideResizeHandles() { m_showResizeHandles = false; }
    void GetResizeHandleCenters(Vec3 centers[6]) const;

    // Displacement brush cursor
    void SetDispBrushCursor(const Vec3& center, const Vec3& normal, float radius);
    void HideDispBrushCursor() { m_showDispCursor = false; }

    // Model placement preview (ghost model)
    void SetModelPreview(const CachedModel* model, const Vec3& posGL,
                         const Vec3& angles = {0,0,0}, float scale = 1.0f);
    void HideModelPreview() { m_showModelPreview = false; }

    // Leak trail (pointfile visualization)
    void SetLeakTrail(const std::vector<Vec3>& pointsGL);
    void ClearLeakTrail();
    bool HasLeakTrail() const { return m_showLeakTrail; }

private:
    void BuildGrid();
    void BuildUnitCube();
    void RebuildBrushMeshes();
    void RebuildEntityMeshes();
    void RebuildRopeMeshes();
    void RebuildModelInstances();
    void DestroyBrushMeshes();
    void DestroyEntityMeshes();
    void DestroyRopeMeshes();
    void RenderModels(const Mat4& vp);
    void RenderModelPreview(const Mat4& vp);
    void RenderSelectionHighlight(const Mat4& vp);

    // Helper to create+draw a temporary wireframe from a float buffer (pos3+color3 interleaved)
    void DrawTempWireframe(const std::vector<float>& verts, float lineWidth = 1.0f);

    Rhi* m_rhi = nullptr;

    RhiShader m_sceneShader = RHI_NULL_SHADER;
    RhiShader m_pickShader  = RHI_NULL_SHADER;

    // Uniform locations (fetched from RHI after shader creation)
    int m_locMVP             = -1;
    int m_locColor           = -1;
    int m_locUseLighting     = -1;
    int m_locUseVertexColor  = -1;
    int m_locUseTexture      = -1;
    int m_locTexSampler      = -1;
    int m_locPickMVP         = -1;

    int m_worldSolidCount = 0;

    // Unit cube for entity AABB pick rendering
    RhiVertexLayout m_cubeVL  = RHI_NULL_VERTEX_LAYOUT;
    RhiBuffer       m_cubeVBO = RHI_NULL_BUFFER;

    TextureCache m_textureCache;
    ModelCache   m_modelCache;

    // Model instances for rendering
    struct ModelInstance
    {
        const CachedModel* model;
        Vec3 position;
        Vec3 angles;  // Euler angles (pitch, yaw, roll)
        float scale = 1.0f;  // modelscale keyvalue
    };
    std::vector<ModelInstance> m_modelInstances;

    RhiVertexLayout m_gridVL  = RHI_NULL_VERTEX_LAYOUT;
    RhiBuffer       m_gridVBO = RHI_NULL_BUFFER;
    int             m_gridVertexCount = 0;

    RhiVertexLayout m_brushVL  = RHI_NULL_VERTEX_LAYOUT;
    RhiBuffer       m_brushVBO = RHI_NULL_BUFFER;
    int             m_brushVertexCount = 0;
    std::vector<DrawBatch> m_drawBatches;

    RhiVertexLayout m_bspWireVL  = RHI_NULL_VERTEX_LAYOUT;
    RhiBuffer       m_bspWireVBO = RHI_NULL_BUFFER;
    int             m_bspWireVertexCount = 0;

    RhiVertexLayout m_dispWireVL  = RHI_NULL_VERTEX_LAYOUT;
    RhiBuffer       m_dispWireVBO = RHI_NULL_BUFFER;
    int             m_dispWireVertexCount = 0;

    // Entity wireframe boxes
    RhiVertexLayout m_entVL  = RHI_NULL_VERTEX_LAYOUT;
    RhiBuffer       m_entVBO = RHI_NULL_BUFFER;
    int             m_entVertexCount = 0;

    // Rope rendering (textured quad strips)
    RhiVertexLayout m_ropeVL  = RHI_NULL_VERTEX_LAYOUT;
    RhiBuffer       m_ropeVBO = RHI_NULL_BUFFER;
    int             m_ropeVertexCount = 0;
    std::vector<DrawBatch> m_ropeDrawBatches;

    VmfDocument* m_doc = nullptr;
    FgdManager*  m_fgd = nullptr;
    bool m_dirty = true;
    int  m_selectedEntityIdx = -1;

    // Preview box state
    bool m_showPreview = false;
    Vec3 m_previewMins;
    Vec3 m_previewMaxs;

    // Clip preview state
    bool m_showClipPreview = false;
    Vec3 m_clipBrushMinsGL;
    Vec3 m_clipBrushMaxsGL;
    int  m_clipGLAxis = 0;
    float m_clipGLAxisPos = 0.0f;
    int  m_clipMode = 0;

    // Resize handles state
    bool m_showResizeHandles = false;
    Vec3 m_resizeMins;
    Vec3 m_resizeMaxs;

    // Displacement brush cursor state
    bool  m_showDispCursor = false;
    Vec3  m_dispCursorCenter;
    Vec3  m_dispCursorNormal;
    float m_dispCursorRadius = 64.0f;

    // Model placement preview state
    bool               m_showModelPreview = false;
    const CachedModel* m_previewModel     = nullptr;
    Vec3               m_previewModelPos;
    Vec3               m_previewModelAngles;
    float              m_previewModelScale = 1.0f;

    // Leak trail state (pointfile)
    bool            m_showLeakTrail = false;
    RhiVertexLayout m_leakVL  = RHI_NULL_VERTEX_LAYOUT;
    RhiBuffer       m_leakVBO = RHI_NULL_BUFFER;
    int             m_leakVertexCount = 0;

    std::function<void(const char*)> m_logFunc;
    void Log(const char* fmt, ...);
};
