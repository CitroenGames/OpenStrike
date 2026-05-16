#pragma once

#include "../ed_math.hpp"
#include "../rhi/rhi_types.hpp"
#include "../scene_renderer.hpp"
#include <imgui.h>
#include <ImGuizmo.h>
#include <cstdint>
#include <functional>

class Rhi;

// Drag-drop payload types (must match content_browser_panel.hpp)
constexpr const char* VIEWPORT_PAYLOAD_MODEL     = "MODEL_PATH";
constexpr const char* VIEWPORT_PAYLOAD_MATERIAL  = "MATERIAL_PATH";

struct Camera
{
    Vec3  position = { 0, 128, 256 };
    float yaw      = -90.0f;
    float pitch    = -15.0f;
};

class VmfDocument;
class FgdManager;
class EntityToolPanel;
class BrushToolPanel;
class ClipToolPanel;
class DisplacementToolPanel;

class ViewportPanel
{
public:
    void Init(Rhi* rhi);
    void Shutdown();
    void Draw();
    void SetDocument(VmfDocument* doc, FgdManager* fgd = nullptr);
    void SetLogFunc(std::function<void(const char*)> fn) { m_logFunc = fn; m_renderer.SetLogFunc(fn); }
    void SetGameDir(const char* dir) { m_renderer.SetGameDir(dir); }
    void MarkDirty() { m_renderer.MarkDirty(); }

    void SetEntityTool(EntityToolPanel* tool) { m_entityTool = tool; }
    void SetBrushTool(BrushToolPanel* tool) { m_brushTool = tool; }
    void SetClipTool(ClipToolPanel* tool) { m_clipTool = tool; }
    void SetDispTool(DisplacementToolPanel* tool) { m_dispTool = tool; }

    bool& IsOpen() { return m_open; }
    ModelCache* GetModelCache() { return &m_renderer.GetModelCache(); }
    SceneRenderer& GetRenderer() { return m_renderer; }

    // Scene change callback (called when entities are added/moved/deleted from viewport)
    using SceneChangedFunc = std::function<void()>;
    void SetSceneChangedFunc(SceneChangedFunc fn) { m_sceneChanged = fn; }

    // Gizmo mode
    ImGuizmo::OPERATION GetGizmoOperation() const { return m_gizmoOp; }
    ImGuizmo::MODE      GetGizmoMode() const      { return m_gizmoMode; }
    void SetGizmoOperation(ImGuizmo::OPERATION op) { m_gizmoOp = op; }
    void SetGizmoMode(ImGuizmo::MODE mode)         { m_gizmoMode = mode; }
    bool IsSnapping() const { return m_gizmoSnap; }
    void SetSnapping(bool s) { m_gizmoSnap = s; }

private:
    void CreateFBO(int width, int height);
    void DestroyFBO();
    void RenderScene();
    void HandleInput();
    void HandleEntityPlacement();
    void HandleBrushCreation();
    void HandleBrushResize();
    void HandleBrushClip();
    void HandleDisplacementTool();
    void UpdateResizeHandles();
    int  PickResizeHandle(const Vec3& rayOrigin, const Vec3& rayDir) const;
    void HandleObjectSelection();
    void HandleObjectMovement();
    void HandleModelDrop(const char* modelPath);
    void HandleMaterialDrop(const char* materialVpkPath);
    void UpdateModelPreview(const char* modelPath, bool isFBX);
    Vec3 ComputePlacementPos(const Vec3& rayOrigin, const Vec3& rayDir, const CachedModel* model);
    void DrawGizmo();
    void DrawGizmoToolbar();

    // Ray casting helpers
    Vec3 ScreenToWorldRay(float screenX, float screenY, int vpWidth, int vpHeight) const;
    int  RayPickEntity(const Vec3& rayOrigin, const Vec3& rayDir, float* outT = nullptr) const;
    bool RayIntersectAABB(const Vec3& origin, const Vec3& dir,
                          const Vec3& bmin, const Vec3& bmax, float& tOut) const;
    bool RayIntersectTriangle(const Vec3& origin, const Vec3& dir,
                              const Vec3& v0, const Vec3& v1, const Vec3& v2,
                              float& t, Vec3& hitPos) const;

    struct RayFaceHit {
        int solidIndex;
        int sideIndex;
        Vec3 hitPos;
        float t;
    };
    bool RayPickBrushFace(const Vec3& rayOrigin, const Vec3& rayDir, RayFaceHit& hit) const;

    struct RayDispHit {
        int solidIndex;
        int sideIndex;
        int triIndex;
        int vertexIndex;
        Vec3 hitPos;        // GL space
        Vec3 vertexPos;     // GL-space nearest displacement vertex
        Vec3 hitNormal;     // GL-space triangle normal
        Vec3 hitPosSource;  // Source space
        float t;
    };
    bool RayPickDisplacement(const Vec3& rayOrigin, const Vec3& rayDir, RayDispHit& hit) const;

    Rhi*           m_rhi       = nullptr;
    RhiFramebuffer m_sceneFB   = RHI_NULL_FRAMEBUFFER;
    RhiFramebuffer m_pickFB    = RHI_NULL_FRAMEBUFFER;
    int            m_fboWidth  = 0;
    int            m_fboHeight = 0;
    bool     m_open         = true;
    bool     m_captured     = false;
    float    m_captureMouseX = 0.0f;
    float    m_captureMouseY = 0.0f;

    Camera        m_camera;
    SceneRenderer m_renderer;
    VmfDocument*  m_doc        = nullptr;
    FgdManager*   m_fgd        = nullptr;
    EntityToolPanel* m_entityTool = nullptr;
    BrushToolPanel*  m_brushTool  = nullptr;
    ClipToolPanel*   m_clipTool   = nullptr;
    DisplacementToolPanel* m_dispTool = nullptr;

    // Brush creation state
    enum class BrushCreationPhase { None, Dragging };
    BrushCreationPhase m_brushPhase = BrushCreationPhase::None;
    Vec3 m_brushStartCorner;
    Vec3 m_brushCurrentCorner;
    float m_brushPlaneDepth = 0.0f;

    // Brush resize state
    enum class ResizePhase { None, Dragging, ClickMode };
    ResizePhase m_resizePhase = ResizePhase::None;
    int m_resizeFaceIndex = -1;       // 0-5: +X,-X,+Y,-Y,+Z,-Z
    Vec3 m_resizeOriginalMins;        // Original brush bounds (GL coords)
    Vec3 m_resizeOriginalMaxs;
    Vec3 m_resizeCurrentMins;         // Current brush bounds (GL coords)
    Vec3 m_resizeCurrentMaxs;
    float m_resizeDragDepth = 0.0f;
    bool m_resizeWasDragged = false;  // Track if mouse moved during drag

    // Clip tool state
    enum class ClipPhase { None, Positioning };
    ClipPhase m_clipPhase = ClipPhase::None;
    float m_clipPlanePos = 0.0f;
    float m_clipDragDepth = 0.0f;
    Vec3  m_clipBrushMinsGL;
    Vec3  m_clipBrushMaxsGL;

    // Movement state
    bool  m_dragging      = false;
    Vec3  m_dragStartPos;
    float m_dragDepth     = 0.0f;

    // Viewport position within the window (for mouse coords)
    float m_vpMinX = 0, m_vpMinY = 0;
    float m_vpMaxX = 0, m_vpMaxY = 0;

    // Cached matrices for picking and gizmo
    Mat4 m_lastView;
    Mat4 m_lastProj;

    // ImGuizmo state
    ImGuizmo::OPERATION m_gizmoOp   = ImGuizmo::TRANSLATE;
    ImGuizmo::MODE      m_gizmoMode = ImGuizmo::WORLD;
    bool m_gizmoSnap = true;
    float m_snapTranslate[3] = { 16.0f, 16.0f, 16.0f };
    float m_snapRotate[3]    = { 15.0f, 15.0f, 15.0f };
    float m_snapScale[3]     = { 0.1f, 0.1f, 0.1f };

    // Model drag preview state
    bool m_modelPreviewActive = false;

    // Displacement tool state
    Vec3 m_dispBrushPos;
    Vec3 m_dispBrushNormal;
    bool m_dispBrushVisible = false;
    bool m_dispSculpting = false;

    std::function<void(const char*)> m_logFunc;
    SceneChangedFunc m_sceneChanged;
};
