#include "viewport_panel.hpp"
#include "entity_tool_panel.hpp"
#include "brush_tool_panel.hpp"
#include "clip_tool_panel.hpp"
#include "displacement_tool_panel.hpp"
#include "../rhi/rhi.hpp"
#include "../brush_mesh_builder.hpp"
#include "../vmf_document.hpp"
#include "../fgd_manager.hpp"

#include <imgui.h>
#include <ImGuizmo.h>
#include <SDL3/SDL.h>
#include <cmath>
#include <cstdio>
#include <cstring>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

static float DegToRad(float d) { return d * (float)(M_PI / 180.0); }


static Vec3 RotateVec3(const Mat4& m, const Vec3& v)
{
    return {
        m.m[0]*v.x + m.m[4]*v.y + m.m[8]*v.z,
        m.m[1]*v.x + m.m[5]*v.y + m.m[9]*v.z,
        m.m[2]*v.x + m.m[6]*v.y + m.m[10]*v.z
    };
}

void ViewportPanel::Init(Rhi* rhi)
{
    m_rhi = rhi;
    m_renderer.Init(rhi);
}

void ViewportPanel::Shutdown()
{
    m_renderer.Shutdown();
    DestroyFBO();
}

void ViewportPanel::SetDocument(VmfDocument* doc, FgdManager* fgd)
{
    m_doc = doc;
    m_fgd = fgd;
    m_renderer.SetDocument(doc, fgd);
}

void ViewportPanel::DrawGizmoToolbar()
{
    // Small toolbar at top-left of viewport for gizmo mode
    ImVec2 vpPos = ImGui::GetCursorScreenPos();

    ImGui::SetCursorScreenPos(ImVec2(m_vpMinX + 4.0f, m_vpMinY + 4.0f));

    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 3.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(2, 2));
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.2f, 0.2f, 0.2f, 0.8f));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.4f, 0.5f, 0.7f, 1.0f));

    auto ToolButton = [&](const char* label, ImGuizmo::OPERATION op) {
        bool active = (m_gizmoOp == op);
        if (active)
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.3f, 0.4f, 0.7f, 1.0f));
        if (ImGui::Button(label, ImVec2(22, 22)))
            m_gizmoOp = op;
        if (active)
            ImGui::PopStyleColor();
        ImGui::SameLine();
    };

    ToolButton("T", ImGuizmo::TRANSLATE);
    ToolButton("R", ImGuizmo::ROTATE);
    ToolButton("S", ImGuizmo::SCALE);

    ImGui::SameLine(0, 6);
    ImGui::TextDisabled("|");
    ImGui::SameLine(0, 6);

    // World/Local toggle
    {
        bool isLocal = (m_gizmoMode == ImGuizmo::LOCAL);
        if (isLocal)
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.3f, 0.4f, 0.7f, 1.0f));
        if (ImGui::Button("L", ImVec2(22, 22)))
            m_gizmoMode = ImGuizmo::LOCAL;
        if (isLocal)
            ImGui::PopStyleColor();
        ImGui::SameLine();

        bool isWorld = (m_gizmoMode == ImGuizmo::WORLD);
        if (isWorld)
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.3f, 0.4f, 0.7f, 1.0f));
        if (ImGui::Button("W", ImVec2(22, 22)))
            m_gizmoMode = ImGuizmo::WORLD;
        if (isWorld)
            ImGui::PopStyleColor();
        ImGui::SameLine();
    }

    ImGui::SameLine(0, 6);
    ImGui::TextDisabled("|");
    ImGui::SameLine(0, 6);

    // Snap toggle
    {
        if (m_gizmoSnap)
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.3f, 0.5f, 0.3f, 1.0f));
        if (ImGui::Button("Snap", ImVec2(36, 22)))
            m_gizmoSnap = !m_gizmoSnap;
        if (m_gizmoSnap)
            ImGui::PopStyleColor();
    }

    ImGui::PopStyleColor(2);
    ImGui::PopStyleVar(2);
}

void ViewportPanel::Draw()
{
    if (!m_open)
        return;

    ImGuizmo::BeginFrame();

    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    ImGui::Begin("3D Viewport", &m_open);
    ImGui::PopStyleVar();

    ImVec2 size = ImGui::GetContentRegionAvail();
    int w = (int)size.x;
    int h = (int)size.y;

    if (w > 0 && h > 0)
    {
        if (w != m_fboWidth || h != m_fboHeight)
            CreateFBO(w, h);

        m_rhi->BindFramebuffer(m_sceneFB);
        RenderScene();
        m_rhi->UnbindFramebuffer();

        // Track viewport screen position for mouse coordinate conversion
        ImVec2 cursorPos = ImGui::GetCursorScreenPos();
        m_vpMinX = cursorPos.x;
        m_vpMinY = cursorPos.y;
        m_vpMaxX = cursorPos.x + size.x;
        m_vpMaxY = cursorPos.y + size.y;

        ImGui::Image(
            m_rhi->GetFramebufferImGuiTexture(m_sceneFB),
            size,
            ImVec2(0, 1), ImVec2(1, 0)
        );

        // Handle drag-drop onto viewport
        if (ImGui::BeginDragDropTarget())
        {
            if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload(
                    VIEWPORT_PAYLOAD_MODEL, ImGuiDragDropFlags_AcceptPeekOnly))
            {
                const char* modelPath = static_cast<const char*>(payload->Data);
                UpdateModelPreview(modelPath, false);
                if (payload->IsDelivery())
                {
                    m_renderer.HideModelPreview();
                    m_modelPreviewActive = false;
                    HandleModelDrop(modelPath);
                }
            }
            else if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload(VIEWPORT_PAYLOAD_MATERIAL))
            {
                const char* materialPath = static_cast<const char*>(payload->Data);
                HandleMaterialDrop(materialPath);
            }
            else
            {
                if (m_modelPreviewActive)
                {
                    m_renderer.HideModelPreview();
                    m_modelPreviewActive = false;
                }
            }
            ImGui::EndDragDropTarget();
        }
        else
        {
            // Drag left viewport -> hide preview
            if (m_modelPreviewActive)
            {
                m_renderer.HideModelPreview();
                m_modelPreviewActive = false;
            }
        }

        // Draw the ImGuizmo gizmo on top of the viewport
        DrawGizmo();

        HandleInput();

        // Draw toolbar overlay on top
        DrawGizmoToolbar();
    }

    ImGui::End();
}

void ViewportPanel::DrawGizmo()
{
    if (!m_doc) return;

    const auto& sel = m_doc->GetSelection();

    // Handle Entity gizmo
    if (sel.type == SelectionType::Entity)
    {
        if (sel.index < 0 || sel.index >= (int)m_doc->GetEntities().size())
            return;

        const auto& ent = m_doc->GetEntities()[sel.index];

        // Build entity model matrix in GL space
        Vec3 srcOrigin = ent.GetOrigin();
        Vec3 glOrigin = SourceToGL(srcOrigin.x, srcOrigin.y, srcOrigin.z);

        // Parse source angles (pitch, yaw, roll)
        Vec3 srcAngles = { 0, 0, 0 };
        const char* anglesStr = ent.GetValue("angles", nullptr);
        if (anglesStr)
            sscanf(anglesStr, "%f %f %f", &srcAngles.x, &srcAngles.y, &srcAngles.z);

        // Source axes map to GL: Z-up→Y, Y-forward→-Z, X-right→X
        // So: yaw→RotY, pitch→RotZ(-pitch), roll→RotX(roll)
        float pitch = srcAngles.x * (float)(M_PI / 180.0);
        float yaw   = srcAngles.y * (float)(M_PI / 180.0);
        float roll  = srcAngles.z * (float)(M_PI / 180.0);

        Mat4 rotation = Mat4::RotationY(yaw) * Mat4::RotationZ(-pitch) * Mat4::RotationX(roll);
        Mat4 translation = Mat4::Translation(glOrigin);
        Mat4 modelMatrix = translation * rotation;

        // Set ImGuizmo to draw in this window's viewport area
        ImGuizmo::SetDrawlist();
        ImGuizmo::SetRect(m_vpMinX, m_vpMinY, m_vpMaxX - m_vpMinX, m_vpMaxY - m_vpMinY);
        ImGuizmo::SetOrthographic(false);

        // Choose snap values based on operation
        float* snapPtr = nullptr;
        if (m_gizmoSnap)
        {
            switch (m_gizmoOp)
            {
            case ImGuizmo::TRANSLATE: snapPtr = m_snapTranslate; break;
            case ImGuizmo::ROTATE:    snapPtr = m_snapRotate;    break;
            case ImGuizmo::SCALE:     snapPtr = m_snapScale;     break;
            default:                  snapPtr = m_snapTranslate; break;
            }
        }

        // Manipulate
        float matrixData[16];
        memcpy(matrixData, modelMatrix.m, sizeof(float) * 16);

        bool manipulated = ImGuizmo::Manipulate(
            m_lastView.Ptr(),
            m_lastProj.Ptr(),
            m_gizmoOp,
            m_gizmoMode,
            matrixData,
            nullptr,
            snapPtr
        );

        if (manipulated)
        {
            // Extract GL position from column-major matrix
            Vec3 newGLPos = { matrixData[12], matrixData[13], matrixData[14] };
            Vec3 newSrcPos = GLToSource(newGLPos);

            m_doc->MoveEntity(sel.index, newSrcPos);

            // Update angles if rotated
            if (m_gizmoOp == ImGuizmo::ROTATE || m_gizmoOp == ImGuizmo::UNIVERSAL)
            {
                // Decompose GL rotation matrix back to Source angles
                // M_gl = RotY(yaw) * RotZ(-pitch) * RotX(roll)
                // m[1] = -sin(pitch), m[5] = cos(pitch)*cos(roll),
                // m[9] = -cos(pitch)*sin(roll), m[0] = cos(yaw)*cos(pitch),
                // m[2] = -sin(yaw)*cos(pitch)
                float srcPitch = asinf(fmaxf(-1.0f, fminf(1.0f, -matrixData[1]))) * (float)(180.0 / M_PI);
                float srcRoll  = atan2f(-matrixData[9], matrixData[5]) * (float)(180.0 / M_PI);
                float srcYaw   = atan2f(-matrixData[2], matrixData[0]) * (float)(180.0 / M_PI);

                char buf[128];
                snprintf(buf, sizeof(buf), "%g %g %g", srcPitch, srcYaw, srcRoll);
                m_doc->SetEntityKeyValue(sel.index, "angles", buf);
            }

            m_renderer.MarkDirty();
            if (m_sceneChanged)
                m_sceneChanged();
        }
    }
    // Handle WorldSolid (brush) gizmo
    else if (sel.type == SelectionType::WorldSolid)
    {
        if (sel.index < 0 || sel.index >= (int)m_doc->GetWorld().solids.size())
            return;

        // Get brush center in Source coords then convert to GL
        Vec3 srcCenter = m_doc->GetWorldSolidCenter(sel.index);
        Vec3 glCenter = SourceToGL(srcCenter.x, srcCenter.y, srcCenter.z);

        // Build model matrix (translation only for brushes - no rotation/scale via gizmo)
        Mat4 modelMatrix = Mat4::Translation(glCenter);

        // Set ImGuizmo to draw in this window's viewport area
        ImGuizmo::SetDrawlist();
        ImGuizmo::SetRect(m_vpMinX, m_vpMinY, m_vpMaxX - m_vpMinX, m_vpMaxY - m_vpMinY);
        ImGuizmo::SetOrthographic(false);

        // Only use translate snap for brushes
        float* snapPtr = nullptr;
        if (m_gizmoSnap)
            snapPtr = m_snapTranslate;

        // Manipulate (only translate for brushes)
        float matrixData[16];
        memcpy(matrixData, modelMatrix.m, sizeof(float) * 16);

        bool manipulated = ImGuizmo::Manipulate(
            m_lastView.Ptr(),
            m_lastProj.Ptr(),
            ImGuizmo::TRANSLATE,  // Only translate for brushes
            m_gizmoMode,
            matrixData,
            nullptr,
            snapPtr
        );

        if (manipulated)
        {
            // Decompose the result matrix
            float newTranslation[3], newRotation[3], newScale[3];
            ImGuizmo::DecomposeMatrixToComponents(matrixData, newTranslation, newRotation, newScale);

            // Calculate delta in GL space
            Vec3 newGLCenter = { newTranslation[0], newTranslation[1], newTranslation[2] };
            Vec3 glDelta = newGLCenter - glCenter;

            // Convert delta to Source coords
            Vec3 srcDelta = GLToSource(glDelta);

            // Move the brush
            m_doc->MoveWorldSolid(sel.index, srcDelta);
            m_renderer.MarkDirty();
            if (m_sceneChanged)
                m_sceneChanged();
        }
    }
}

void ViewportPanel::HandleInput()
{
    ImGuiIO& io = ImGui::GetIO();
    bool hovered = ImGui::IsItemHovered();

    // Don't process click-selection/placement while ImGuizmo is active
    bool gizmoActive = ImGuizmo::IsUsing() || ImGuizmo::IsOver();

    // Keyboard shortcuts for gizmo modes (when viewport is hovered)
    if (hovered && !m_captured)
    {
        if (ImGui::IsKeyPressed(ImGuiKey_1))
            m_gizmoOp = ImGuizmo::TRANSLATE;
        if (ImGui::IsKeyPressed(ImGuiKey_2))
            m_gizmoOp = ImGuizmo::ROTATE;
        if (ImGui::IsKeyPressed(ImGuiKey_3))
            m_gizmoOp = ImGuizmo::SCALE;
        if (ImGui::IsKeyPressed(ImGuiKey_4))
            m_gizmoOp = ImGuizmo::UNIVERSAL;
        if (ImGui::IsKeyPressed(ImGuiKey_X))
            m_gizmoMode = (m_gizmoMode == ImGuizmo::WORLD) ? ImGuizmo::LOCAL : ImGuizmo::WORLD;
        if (ImGui::IsKeyPressed(ImGuiKey_G))
            m_gizmoSnap = !m_gizmoSnap;
    }

    // Brush creation (click-drag when brush tool is active)
    if (hovered && !gizmoActive && m_brushTool && m_brushTool->IsCreationActive())
    {
        HandleBrushCreation();
        if (m_brushPhase != BrushCreationPhase::None)
            return;
    }

    // Clip tool (when active and a brush is selected)
    if (hovered && !gizmoActive && m_clipTool && m_clipTool->IsClipActive())
    {
        const auto& sel = m_doc->GetSelection();
        if (sel.type == SelectionType::WorldSolid && sel.index >= 0
            && sel.index < (int)m_doc->GetWorld().solids.size())
        {
            HandleBrushClip();
            if (m_clipPhase != ClipPhase::None)
                return;
        }
    }

    // Displacement tool (when active)
    if (hovered && !gizmoActive && m_dispTool && m_dispTool->IsActive())
    {
        HandleDisplacementTool();
        return;
    }

    // Update resize handles for selected brushes
    UpdateResizeHandles();

    // Brush resize (when handles are shown and being dragged)
    if (hovered && !gizmoActive && m_resizePhase != ResizePhase::None)
    {
        HandleBrushResize();
        return;
    }

    // Check for resize handle click
    if (hovered && !gizmoActive && ImGui::IsMouseClicked(ImGuiMouseButton_Left) && !m_captured)
    {
        ImVec2 mousePos = io.MousePos;
        float localX = mousePos.x - m_vpMinX;
        float localY = mousePos.y - m_vpMinY;
        Vec3 rayDir = ScreenToWorldRay(localX, localY, m_fboWidth, m_fboHeight);
        Vec3 rayOrigin = m_camera.position;

        int handle = PickResizeHandle(rayOrigin, rayDir);
        if (handle >= 0)
        {
            HandleBrushResize();
            return;
        }
    }

    // Entity placement (left click when tool is active)
    if (hovered && !gizmoActive && m_entityTool && m_entityTool->IsPlacementActive()
        && ImGui::IsMouseClicked(ImGuiMouseButton_Left) && !io.KeyCtrl)
    {
        HandleEntityPlacement();
        return;
    }

    // Object selection (left click, but not when gizmo is active)
    if (hovered && !gizmoActive && ImGui::IsMouseClicked(ImGuiMouseButton_Left) && !m_captured)
    {
        HandleObjectSelection();
    }

    // Object movement (keyboard/middle mouse, but not when gizmo is in use)
    if (!ImGuizmo::IsUsing())
        HandleObjectMovement();

    // Camera control (right click drag)
    if (hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Right))
    {
        m_captured = true;
        SDL_GetGlobalMouseState(&m_captureMouseX, &m_captureMouseY);
        if (SDL_Window* window = SDL_GetMouseFocus())
            SDL_SetWindowRelativeMouseMode(window, true);
        SDL_GetRelativeMouseState(nullptr, nullptr);  // flush stale delta
    }

    if (m_captured && !ImGui::IsMouseDown(ImGuiMouseButton_Right))
    {
        m_captured = false;
        if (SDL_Window* window = SDL_GetMouseFocus())
            SDL_SetWindowRelativeMouseMode(window, false);
        SDL_WarpMouseGlobal(m_captureMouseX, m_captureMouseY);
    }

    if (!m_captured)
        return;

    float relX = 0.0f, relY = 0.0f;
    SDL_GetRelativeMouseState(&relX, &relY);
    float sensitivity = 0.15f;
    m_camera.yaw   += relX * sensitivity;
    m_camera.pitch -= relY * sensitivity;

    if (m_camera.pitch >  89.0f) m_camera.pitch =  89.0f;
    if (m_camera.pitch < -89.0f) m_camera.pitch = -89.0f;

    float yawRad   = DegToRad(m_camera.yaw);
    float pitchRad = DegToRad(m_camera.pitch);

    Vec3 forward;
    forward.x = cosf(pitchRad) * cosf(yawRad);
    forward.y = sinf(pitchRad);
    forward.z = cosf(pitchRad) * sinf(yawRad);

    Vec3 right;
    right.x = cosf(yawRad + (float)(M_PI * 0.5));
    right.y = 0.0f;
    right.z = sinf(yawRad + (float)(M_PI * 0.5));

    Vec3 up = { 0, 1, 0 };

    float speed = 200.0f * io.DeltaTime;
    if (io.KeyShift) speed *= 3.0f;

    if (ImGui::IsKeyDown(ImGuiKey_W))
        m_camera.position += forward * speed;
    if (ImGui::IsKeyDown(ImGuiKey_S))
        m_camera.position -= forward * speed;
    if (ImGui::IsKeyDown(ImGuiKey_D))
        m_camera.position += right * speed;
    if (ImGui::IsKeyDown(ImGuiKey_A))
        m_camera.position -= right * speed;
    if (ImGui::IsKeyDown(ImGuiKey_Space))
        m_camera.position += up * speed;
    if (ImGui::IsKeyDown(ImGuiKey_LeftCtrl))
        m_camera.position -= up * speed;
}

// ---- Ray Casting ----

Vec3 ViewportPanel::ScreenToWorldRay(float screenX, float screenY, int vpWidth, int vpHeight) const
{
    float ndcX = (2.0f * screenX / vpWidth) - 1.0f;
    float ndcY = 1.0f - (2.0f * screenY / vpHeight);

    float fovRad = 70.0f * (float)(M_PI / 180.0f);
    float aspect = (float)vpWidth / (float)vpHeight;
    float tanHalfFov = tanf(fovRad * 0.5f);

    Vec3 rayView;
    rayView.x = ndcX * aspect * tanHalfFov;
    rayView.y = ndcY * tanHalfFov;
    rayView.z = -1.0f;

    float yawRad   = DegToRad(m_camera.yaw);
    float pitchRad = DegToRad(m_camera.pitch);

    Vec3 forward;
    forward.x = cosf(pitchRad) * cosf(yawRad);
    forward.y = sinf(pitchRad);
    forward.z = cosf(pitchRad) * sinf(yawRad);

    Vec3 worldRight;
    worldRight.x = cosf(yawRad + (float)(M_PI * 0.5));
    worldRight.y = 0.0f;
    worldRight.z = sinf(yawRad + (float)(M_PI * 0.5));

    Vec3 worldUp = Cross(worldRight, forward);

    Vec3 dir = worldRight * rayView.x + worldUp * rayView.y + forward * (-rayView.z);
    return Normalize(dir);
}

bool ViewportPanel::RayIntersectAABB(const Vec3& origin, const Vec3& dir,
                                      const Vec3& bmin, const Vec3& bmax, float& tOut) const
{
    float tmin = -1e30f;
    float tmax =  1e30f;

    for (int i = 0; i < 3; i++)
    {
        float o = (&origin.x)[i];
        float d = (&dir.x)[i];
        float lo = (&bmin.x)[i];
        float hi = (&bmax.x)[i];

        if (fabsf(d) < 1e-8f)
        {
            if (o < lo || o > hi)
                return false;
        }
        else
        {
            float t1 = (lo - o) / d;
            float t2 = (hi - o) / d;
            if (t1 > t2) { float tmp = t1; t1 = t2; t2 = tmp; }
            if (t1 > tmin) tmin = t1;
            if (t2 < tmax) tmax = t2;
            if (tmin > tmax) return false;
        }
    }

    if (tmax < 0) return false;
    tOut = tmin > 0 ? tmin : tmax;
    return true;
}

int ViewportPanel::RayPickEntity(const Vec3& rayOrigin, const Vec3& rayDir, float* outT) const
{
    if (!m_doc) return -1;

    float bestT = 1e30f;
    int bestIdx = -1;
    float defaultSize = 8.0f;

    const auto& entities = m_doc->GetEntities();
    for (int i = 0; i < (int)entities.size(); i++)
    {
        const auto& ent = entities[i];
        if (!ent.solids.empty()) continue;

        Vec3 srcOrigin = ent.GetOrigin();
        Vec3 origin = SourceToGL(srcOrigin.x, srcOrigin.y, srcOrigin.z);

        Vec3 mins = { -defaultSize, -defaultSize, -defaultSize };
        Vec3 maxs = {  defaultSize,  defaultSize,  defaultSize };

        // Use model hull bounds if entity has a model
        const char* modelPath = ent.GetValue("model", nullptr);
        if (modelPath && modelPath[0] != '\0' && modelPath[0] != '*')
        {
            const CachedModel* cachedModel = m_renderer.GetModelCache().Find(modelPath);
            if (cachedModel)
            {
                float scale = 1.0f;
                const char* scaleStr = ent.GetValue("modelscale", nullptr);
                if (scaleStr)
                    scale = (float)atof(scaleStr);
                if (scale <= 0.0f) scale = 1.0f;

                mins = cachedModel->mins * scale;
                maxs = cachedModel->maxs * scale;
            }
        }

        // Build rotation from entity angles and transform ray into local space
        Vec3 angles = {0, 0, 0};
        const char* anglesStr = ent.GetValue("angles", nullptr);
        if (anglesStr)
            sscanf(anglesStr, "%f %f %f", &angles.x, &angles.y, &angles.z);
        float pitch = angles.x * (float)(M_PI / 180.0);
        float yaw   = angles.y * (float)(M_PI / 180.0);
        float roll  = angles.z * (float)(M_PI / 180.0);
        Mat4 rot = Mat4::RotationY(yaw) * Mat4::RotationZ(-pitch) * Mat4::RotationX(roll);

        // Inverse rotation = transpose for pure rotation matrices
        Mat4 invRot;
        for (int ri = 0; ri < 4; ri++)
            for (int ci = 0; ci < 4; ci++)
                invRot.m[ci*4+ri] = rot.m[ri*4+ci];

        Vec3 localRayOrigin = RotateVec3(invRot, rayOrigin - origin);
        Vec3 localRayDir = RotateVec3(invRot, rayDir);

        float t = 0;
        if (RayIntersectAABB(localRayOrigin, localRayDir, mins, maxs, t))
        {
            if (t < bestT)
            {
                bestT = t;
                bestIdx = i;
            }
        }
    }

    if (outT) *outT = bestT;
    return bestIdx;
}

// ---- Model Drop from Content Browser ----

void ViewportPanel::HandleModelDrop(const char* modelPath)
{
    if (!m_doc || !modelPath || modelPath[0] == '\0')
        return;

    // Get mouse position relative to viewport
    ImVec2 mousePos = ImGui::GetIO().MousePos;
    float localX = mousePos.x - m_vpMinX;
    float localY = mousePos.y - m_vpMinY;

    // Validate mouse is within viewport bounds
    if (localX < 0 || localY < 0 ||
        localX > (m_vpMaxX - m_vpMinX) ||
        localY > (m_vpMaxY - m_vpMinY))
        return;

    // Calculate world position using ray-cast against surfaces
    Vec3 rayDir = ScreenToWorldRay(localX, localY, m_fboWidth, m_fboHeight);
    Vec3 rayOrigin = m_camera.position;

    // Load model to get bounding box for surface offset
    const CachedModel& cachedModel = m_renderer.GetModelCache().Get(modelPath);
    const CachedModel* modelPtr = cachedModel.valid ? &cachedModel : nullptr;

    Vec3 glPos = ComputePlacementPos(rayOrigin, rayDir, modelPtr);
    Vec3 srcPos = GLToSource(glPos);

    // Snap to 64-unit grid
    srcPos.x = floorf(srcPos.x / 64.0f + 0.5f) * 64.0f;
    srcPos.y = floorf(srcPos.y / 64.0f + 0.5f) * 64.0f;
    srcPos.z = floorf(srcPos.z / 64.0f + 0.5f) * 64.0f;

    // Create prop_static entity
    int idx = m_doc->AddEntity("prop_static", srcPos);
    m_doc->ApplyFgdDefaults(idx, m_fgd);

    // Set the model path keyvalue
    m_doc->SetEntityKeyValue(idx, "model", modelPath);

    // Select the newly created entity
    m_doc->SetSelection({ SelectionType::Entity, idx, -1 });

    // Trigger scene rebuild to render the new model
    m_renderer.MarkDirty();

    // Notify scene changed (updates outliner, properties panel, etc.)
    if (m_sceneChanged)
        m_sceneChanged();

    // Log the action
    if (m_logFunc)
    {
        char buf[512];
        snprintf(buf, sizeof(buf), "Placed prop_static with model '%s' at (%.0f, %.0f, %.0f)",
                 modelPath, srcPos.x, srcPos.y, srcPos.z);
        m_logFunc(buf);
    }
}

Vec3 ViewportPanel::ComputePlacementPos(const Vec3& rayOrigin, const Vec3& rayDir,
                                         const CachedModel* model)
{
    Vec3 glPos;
    bool hitSurface = false;
    float bestT = 1e30f;

    // Try brush face raycast
    RayFaceHit faceHit;
    if (RayPickBrushFace(rayOrigin, rayDir, faceHit))
    {
        glPos = faceHit.hitPos;
        bestT = faceHit.t;
        hitSurface = true;
    }

    // Try displacement raycast - use closest hit
    RayDispHit dispHit;
    if (RayPickDisplacement(rayOrigin, rayDir, dispHit))
    {
        if (!hitSurface || dispHit.t < bestT)
        {
            glPos = dispHit.hitPos;
            bestT = dispHit.t;
            hitSurface = true;
        }
    }

    if (hitSurface)
    {
        // Offset so model's bottom rests on the surface
        // model->mins.y is the lowest point in GL space (Y-up)
        if (model && model->valid)
            glPos.y -= model->mins.y;
    }
    else
    {
        // Fallback: place at fixed distance from camera
        float placeDist = 200.0f;
        glPos = rayOrigin + rayDir * placeDist;
    }

    return glPos;
}

void ViewportPanel::UpdateModelPreview(const char* modelPath, bool /*isFBX*/)
{
    if (!modelPath || modelPath[0] == '\0')
        return;

    // Get or load the model from cache (always MDL path now)
    const CachedModel& m = m_renderer.GetModelCache().Get(modelPath);
    const CachedModel* model = m.valid ? &m : nullptr;

    if (!model)
        return;

    // Get mouse position relative to viewport
    ImVec2 mousePos = ImGui::GetIO().MousePos;
    float localX = mousePos.x - m_vpMinX;
    float localY = mousePos.y - m_vpMinY;

    if (localX < 0 || localY < 0 ||
        localX > (m_vpMaxX - m_vpMinX) ||
        localY > (m_vpMaxY - m_vpMinY))
    {
        m_renderer.HideModelPreview();
        m_modelPreviewActive = false;
        return;
    }

    // Calculate ray
    Vec3 rayDir = ScreenToWorldRay(localX, localY, m_fboWidth, m_fboHeight);
    Vec3 rayOrigin = m_camera.position;

    // Compute placement position with surface raycasting
    Vec3 glPos = ComputePlacementPos(rayOrigin, rayDir, model);

    // Snap to grid (Source space, then back)
    Vec3 srcPos = GLToSource(glPos);
    srcPos.x = floorf(srcPos.x / 64.0f + 0.5f) * 64.0f;
    srcPos.y = floorf(srcPos.y / 64.0f + 0.5f) * 64.0f;
    srcPos.z = floorf(srcPos.z / 64.0f + 0.5f) * 64.0f;
    glPos = SourceToGL(srcPos.x, srcPos.y, srcPos.z);

    // Update renderer preview
    m_renderer.SetModelPreview(model, glPos);
    m_modelPreviewActive = true;
}

void ViewportPanel::HandleMaterialDrop(const char* materialVpkPath)
{
    if (!m_doc || !materialVpkPath || materialVpkPath[0] == '\0')
        return;
    if (!m_pickFB || m_fboWidth <= 0 || m_fboHeight <= 0)
        return;

    // Convert VPK path to material name:
    // "materials/dev/dev_measuregeneric01.vmt" -> "dev/dev_measuregeneric01"
    std::string matName(materialVpkPath);
    // Normalize separators
    for (char& c : matName)
    {
        if (c == '\\') c = '/';
        c = (char)tolower((unsigned char)c);
    }
    // Strip "materials/" prefix
    const char* prefix = "materials/";
    size_t prefixLen = strlen(prefix);
    if (matName.size() > prefixLen && matName.compare(0, prefixLen, prefix) == 0)
        matName = matName.substr(prefixLen);
    // Strip ".vmt" extension
    if (matName.size() > 4 && matName.compare(matName.size() - 4, 4, ".vmt") == 0)
        matName = matName.substr(0, matName.size() - 4);

    // Get mouse position relative to viewport
    ImVec2 mousePos = ImGui::GetIO().MousePos;
    int px = (int)(mousePos.x - m_vpMinX);
    int py = (int)(mousePos.y - m_vpMinY);

    if (px < 0 || px >= m_fboWidth || py < 0 || py >= m_fboHeight)
        return;

    // GPU picking: identify which solid is under the cursor
    m_rhi->BindFramebuffer(m_pickFB);
    m_rhi->SetViewport(0, 0, m_fboWidth, m_fboHeight);
    m_rhi->Clear(0.0f, 0.0f, 0.0f, 0.0f);
    m_renderer.RenderPick(m_lastView, m_lastProj);

    int glY = m_fboHeight - 1 - py;
    unsigned char pixel[4] = {};
    m_rhi->ReadPixel(m_pickFB, px, glY, pixel);
    m_rhi->UnbindFramebuffer();

    int id = pixel[0] | (pixel[1] << 8);
    if (id == 0)
        return;

    int worldCount = m_renderer.GetWorldSolidCount();

    // Resolve pick ID to a mutable solid reference
    VmfSolid* solid = nullptr;
    if (id >= 1 && id <= worldCount)
    {
        int solidIdx = id - 1;
        auto& solids = m_doc->GetWorldMut().solids;
        if (solidIdx < (int)solids.size())
            solid = &solids[solidIdx];
    }
    else
    {
        // Entity brush solids: pick IDs are worldCount+1.. assigned sequentially
        int flatIdx = id - worldCount - 1;
        auto& entities = m_doc->GetEntitiesMut();
        for (auto& ent : entities)
        {
            if (flatIdx < (int)ent.solids.size())
            {
                solid = &ent.solids[flatIdx];
                break;
            }
            flatIdx -= (int)ent.solids.size();
        }
    }

    if (!solid)
        return;

    bool applyAll = ImGui::GetIO().KeyCtrl;

    if (applyAll)
    {
        // Apply to all faces of the brush
        for (auto& side : solid->sides)
            side.material = matName;

        if (m_logFunc)
        {
            char buf[512];
            snprintf(buf, sizeof(buf), "Applied material '%s' to all faces of solid %d",
                     matName.c_str(), solid->id);
            m_logFunc(buf);
        }
    }
    else
    {
        // Ray cast to find the specific face
        float localX = mousePos.x - m_vpMinX;
        float localY = mousePos.y - m_vpMinY;
        Vec3 rayDir = ScreenToWorldRay(localX, localY, m_fboWidth, m_fboHeight);
        Vec3 rayOrigin = m_camera.position;

        BrushMesh mesh = BuildBrushMesh(*solid);

        float bestT = 1e30f;
        int bestSideIdx = -1;

        for (const auto& face : mesh.faces)
        {
            if (face.vertices.size() < 3)
                continue;

            // Fan-triangulate and test ray intersection
            for (size_t vi = 1; vi + 1 < face.vertices.size(); vi++)
            {
                float t;
                Vec3 hp;
                if (RayIntersectTriangle(rayOrigin, rayDir,
                    face.vertices[0].pos, face.vertices[vi].pos, face.vertices[vi + 1].pos,
                    t, hp))
                {
                    if (t < bestT)
                    {
                        bestT = t;
                        // Match face back to VmfSide by material
                        for (int s = 0; s < (int)solid->sides.size(); s++)
                        {
                            if (solid->sides[s].material == face.material &&
                                !solid->sides[s].dispinfo.has_value())
                            {
                                bestSideIdx = s;
                                break;
                            }
                        }
                    }
                }
            }
        }

        if (bestSideIdx < 0)
            return;

        solid->sides[bestSideIdx].material = matName;

        if (m_logFunc)
        {
            char buf[512];
            snprintf(buf, sizeof(buf), "Applied material '%s' to face %d of solid %d",
                     matName.c_str(), bestSideIdx, solid->id);
            m_logFunc(buf);
        }
    }

    m_doc->MarkDirty();
    m_renderer.MarkDirty();
    if (m_sceneChanged)
        m_sceneChanged();
}

// ---- Entity Placement ----

void ViewportPanel::HandleEntityPlacement()
{
    if (!m_doc || !m_entityTool) return;

    ImVec2 mousePos = ImGui::GetIO().MousePos;
    float localX = mousePos.x - m_vpMinX;
    float localY = mousePos.y - m_vpMinY;

    Vec3 rayDir = ScreenToWorldRay(localX, localY, m_fboWidth, m_fboHeight);
    Vec3 rayOrigin = m_camera.position;

    float placeDist = 200.0f;
    Vec3 glPos = rayOrigin + rayDir * placeDist;
    Vec3 srcPos = GLToSource(glPos);

    srcPos.x = floorf(srcPos.x / 64.0f + 0.5f) * 64.0f;
    srcPos.y = floorf(srcPos.y / 64.0f + 0.5f) * 64.0f;
    srcPos.z = floorf(srcPos.z / 64.0f + 0.5f) * 64.0f;

    int idx = m_doc->AddEntity(m_entityTool->GetSelectedClass(), srcPos);
    m_doc->ApplyFgdDefaults(idx, m_fgd);
    m_doc->SetSelection({ SelectionType::Entity, idx, -1 });

    m_renderer.MarkDirty();
    if (m_sceneChanged)
        m_sceneChanged();

    m_entityTool->OnEntityPlaced();

    if (m_logFunc)
    {
        char buf[256];
        snprintf(buf, sizeof(buf), "Placed %s at (%.0f, %.0f, %.0f)",
            m_entityTool->GetSelectedClass().c_str(), srcPos.x, srcPos.y, srcPos.z);
        m_logFunc(buf);
    }
}

// ---- Brush Creation ----

void ViewportPanel::HandleBrushCreation()
{
    if (!m_doc || !m_brushTool) return;

    ImGuiIO& io = ImGui::GetIO();
    ImVec2 mousePos = io.MousePos;
    float localX = mousePos.x - m_vpMinX;
    float localY = mousePos.y - m_vpMinY;

    Vec3 rayDir = ScreenToWorldRay(localX, localY, m_fboWidth, m_fboHeight);
    Vec3 rayOrigin = m_camera.position;

    float gridSize = m_brushTool->GetGridSize();

    if (m_brushPhase == BrushCreationPhase::None)
    {
        if (ImGui::IsMouseClicked(ImGuiMouseButton_Left))
        {
            m_brushPlaneDepth = 200.0f;
            Vec3 glPos = rayOrigin + rayDir * m_brushPlaneDepth;
            Vec3 srcPos = GLToSource(glPos);

            srcPos.x = floorf(srcPos.x / gridSize + 0.5f) * gridSize;
            srcPos.y = floorf(srcPos.y / gridSize + 0.5f) * gridSize;
            srcPos.z = floorf(srcPos.z / gridSize + 0.5f) * gridSize;

            m_brushStartCorner = srcPos;
            m_brushCurrentCorner = srcPos;
            m_brushPhase = BrushCreationPhase::Dragging;
        }
    }
    else if (m_brushPhase == BrushCreationPhase::Dragging)
    {
        Vec3 glPos = rayOrigin + rayDir * m_brushPlaneDepth;
        Vec3 srcPos = GLToSource(glPos);

        srcPos.x = floorf(srcPos.x / gridSize + 0.5f) * gridSize;
        srcPos.y = floorf(srcPos.y / gridSize + 0.5f) * gridSize;
        srcPos.z = m_brushStartCorner.z;

        m_brushCurrentCorner = srcPos;

        // Calculate preview bounds in Source coords
        Vec3 mins, maxs;
        mins.x = fminf(m_brushStartCorner.x, m_brushCurrentCorner.x);
        mins.y = fminf(m_brushStartCorner.y, m_brushCurrentCorner.y);
        maxs.x = fmaxf(m_brushStartCorner.x, m_brushCurrentCorner.x);
        maxs.y = fmaxf(m_brushStartCorner.y, m_brushCurrentCorner.y);
        mins.z = m_brushStartCorner.z;
        maxs.z = m_brushStartCorner.z + m_brushTool->GetDefaultHeight();

        // Convert to GL coords for preview rendering
        Vec3 glMins = SourceToGL(mins.x, mins.y, mins.z);
        Vec3 glMaxs = SourceToGL(maxs.x, maxs.y, maxs.z);
        // Ensure mins < maxs after coordinate conversion
        if (glMins.x > glMaxs.x) { float t = glMins.x; glMins.x = glMaxs.x; glMaxs.x = t; }
        if (glMins.y > glMaxs.y) { float t = glMins.y; glMins.y = glMaxs.y; glMaxs.y = t; }
        if (glMins.z > glMaxs.z) { float t = glMins.z; glMins.z = glMaxs.z; glMaxs.z = t; }

        m_renderer.SetPreviewBox(glMins, glMaxs);

        if (ImGui::IsMouseReleased(ImGuiMouseButton_Left))
        {
            m_renderer.HidePreview();

            if (maxs.x - mins.x >= gridSize && maxs.y - mins.y >= gridSize)
            {
                VmfSolid solid = VmfDocument::CreateBoxSolid(
                    0, mins, maxs, m_brushTool->GetDefaultMaterial());

                int idx = m_doc->AddWorldSolid(solid);
                m_doc->SetSelection({ SelectionType::WorldSolid, idx, -1 });

                m_renderer.MarkDirty();
                if (m_sceneChanged)
                    m_sceneChanged();

                if (m_logFunc)
                {
                    char buf[256];
                    snprintf(buf, sizeof(buf),
                        "Created box brush at (%.0f, %.0f, %.0f) - (%.0f, %.0f, %.0f)",
                        mins.x, mins.y, mins.z, maxs.x, maxs.y, maxs.z);
                    m_logFunc(buf);
                }

                m_brushTool->OnBrushCreated();
            }

            m_brushPhase = BrushCreationPhase::None;
        }

        if (ImGui::IsMouseClicked(ImGuiMouseButton_Right) ||
            ImGui::IsKeyPressed(ImGuiKey_Escape))
        {
            m_renderer.HidePreview();
            m_brushPhase = BrushCreationPhase::None;
        }
    }
}

// ---- Brush Clipping ----

void ViewportPanel::HandleBrushClip()
{
    if (!m_doc || !m_clipTool) return;

    const auto& sel = m_doc->GetSelection();
    if (sel.type != SelectionType::WorldSolid || sel.index < 0)
    {
        m_clipPhase = ClipPhase::None;
        return;
    }

    ImGuiIO& io = ImGui::GetIO();
    ImVec2 mousePos = io.MousePos;
    float localX = mousePos.x - m_vpMinX;
    float localY = mousePos.y - m_vpMinY;

    Vec3 rayDir = ScreenToWorldRay(localX, localY, m_fboWidth, m_fboHeight);
    Vec3 rayOrigin = m_camera.position;

    float gridSize = m_clipTool->GetGridSize();

    // Map ClipAxis (Source space) to GL axis index
    // Source X -> GL X (0), Source Y -> GL Z (2), Source Z -> GL Y (1)
    auto sourceAxisToGL = [](ClipAxis a) -> int {
        switch (a)
        {
        case ClipAxis::X: return 0;
        case ClipAxis::Y: return 2;
        case ClipAxis::Z: return 1;
        }
        return 0;
    };

    if (m_clipPhase == ClipPhase::None)
    {
        if (ImGui::IsMouseClicked(ImGuiMouseButton_Left))
        {
            // Compute brush bounds in GL space
            const auto& solids = m_doc->GetWorld().solids;
            VmfDocument::ComputeSolidBoundsGL(solids[sel.index], m_clipBrushMinsGL, m_clipBrushMaxsGL);

            // Calculate depth to brush center
            Vec3 brushCenter = (m_clipBrushMinsGL + m_clipBrushMaxsGL) * 0.5f;
            Vec3 toBrush = brushCenter - m_camera.position;
            float yawRad = m_camera.yaw * (float)(M_PI / 180.0);
            float pitchRad = m_camera.pitch * (float)(M_PI / 180.0);
            Vec3 forward;
            forward.x = cosf(pitchRad) * cosf(yawRad);
            forward.y = sinf(pitchRad);
            forward.z = cosf(pitchRad) * sinf(yawRad);
            m_clipDragDepth = Dot(toBrush, forward);
            if (m_clipDragDepth < 10.0f) m_clipDragDepth = 200.0f;

            // Get initial position from ray
            Vec3 hitGL = rayOrigin + rayDir * m_clipDragDepth;
            Vec3 hitSrc = GLToSource(hitGL);

            // Extract Source-space axis component and snap
            int srcAxis = (int)m_clipTool->GetAxis();
            float axisVal = (&hitSrc.x)[srcAxis];
            axisVal = floorf(axisVal / gridSize + 0.5f) * gridSize;
            m_clipPlanePos = axisVal;

            m_clipPhase = ClipPhase::Positioning;
        }
    }
    else if (m_clipPhase == ClipPhase::Positioning)
    {
        // Update position from mouse
        Vec3 hitGL = rayOrigin + rayDir * m_clipDragDepth;
        Vec3 hitSrc = GLToSource(hitGL);

        int srcAxis = (int)m_clipTool->GetAxis();
        float axisVal = (&hitSrc.x)[srcAxis];
        axisVal = floorf(axisVal / gridSize + 0.5f) * gridSize;

        // Clamp to brush bounds in Source space
        Vec3 brushMinsSrc = GLToSource(m_clipBrushMinsGL);
        Vec3 brushMaxsSrc = GLToSource(m_clipBrushMaxsGL);
        // Ensure mins < maxs after conversion
        for (int i = 0; i < 3; i++)
        {
            if ((&brushMinsSrc.x)[i] > (&brushMaxsSrc.x)[i])
            {
                float t = (&brushMinsSrc.x)[i];
                (&brushMinsSrc.x)[i] = (&brushMaxsSrc.x)[i];
                (&brushMaxsSrc.x)[i] = t;
            }
        }

        float srcMin = (&brushMinsSrc.x)[srcAxis];
        float srcMax = (&brushMaxsSrc.x)[srcAxis];
        if (axisVal < srcMin) axisVal = srcMin;
        if (axisVal > srcMax) axisVal = srcMax;
        m_clipPlanePos = axisVal;

        // Convert clip position to GL for preview
        int glAxis = sourceAxisToGL(m_clipTool->GetAxis());
        // Build a Source-space point with the clip value on the right axis
        Vec3 srcPoint = {0, 0, 0};
        (&srcPoint.x)[srcAxis] = m_clipPlanePos;
        Vec3 glPoint = SourceToGL(srcPoint.x, srcPoint.y, srcPoint.z);
        float glAxisPos = (&glPoint.x)[glAxis];

        m_renderer.SetClipPreview(m_clipBrushMinsGL, m_clipBrushMaxsGL,
                                  glAxis, glAxisPos, (int)m_clipTool->GetMode());

        // Tab cycles mode
        if (ImGui::IsKeyPressed(ImGuiKey_Tab))
            m_clipTool->CycleClipMode();

        // C cycles axis
        if (ImGui::IsKeyPressed(ImGuiKey_C))
            m_clipTool->CycleClipAxis();

        // Left click confirms the clip
        if (ImGui::IsMouseClicked(ImGuiMouseButton_Left))
        {
            m_renderer.HideClipPreview();

            int result = m_doc->ClipWorldSolid(
                sel.index,
                (int)m_clipTool->GetAxis(),
                m_clipPlanePos,
                (int)m_clipTool->GetMode(),
                m_clipTool->GetClipMaterial()
            );

            if (result >= 0)
            {
                m_renderer.MarkDirty();
                m_doc->MarkDirty();
                if (m_sceneChanged)
                    m_sceneChanged();

                if (m_logFunc)
                {
                    const char* axisNames[] = { "X", "Y", "Z" };
                    const char* modeNames[] = { "Both", "Front", "Back" };
                    char buf[256];
                    snprintf(buf, sizeof(buf), "Clipped brush on %s axis at %.0f (keep %s)",
                        axisNames[(int)m_clipTool->GetAxis()], m_clipPlanePos,
                        modeNames[(int)m_clipTool->GetMode()]);
                    m_logFunc(buf);
                }
            }

            m_clipTool->ClearClip();
            m_clipPhase = ClipPhase::None;
        }

        // Right click or Escape cancels
        if (ImGui::IsMouseClicked(ImGuiMouseButton_Right) ||
            ImGui::IsKeyPressed(ImGuiKey_Escape))
        {
            m_renderer.HideClipPreview();
            m_clipTool->ClearClip();
            m_clipPhase = ClipPhase::None;
        }
    }
}

// ---- Object Selection (GPU Picking) ----

void ViewportPanel::HandleObjectSelection()
{
    if (!m_doc) return;
    if (!m_pickFB || m_fboWidth <= 0 || m_fboHeight <= 0) return;

    ImVec2 mousePos = ImGui::GetIO().MousePos;
    int px = (int)(mousePos.x - m_vpMinX);
    int py = (int)(mousePos.y - m_vpMinY);

    // Bounds check
    if (px < 0 || px >= m_fboWidth || py < 0 || py >= m_fboHeight)
    {
        m_doc->ClearSelection();
        return;
    }

    // Render pick buffer
    m_rhi->BindFramebuffer(m_pickFB);
    m_rhi->SetViewport(0, 0, m_fboWidth, m_fboHeight);
    m_rhi->Clear(0.0f, 0.0f, 0.0f, 0.0f);
    m_renderer.RenderPick(m_lastView, m_lastProj);

    // Read pixel at mouse position (flip Y for OpenGL)
    int glY = m_fboHeight - 1 - py;
    unsigned char pixel[4] = {};
    m_rhi->ReadPixel(m_pickFB, px, glY, pixel);
    m_rhi->UnbindFramebuffer();

    // Decode pick ID from RG channels
    int id = pixel[0] | (pixel[1] << 8);
    int worldCount = m_renderer.GetWorldSolidCount();

    if (id == 0)
    {
        m_doc->ClearSelection();
    }
    else if (id <= worldCount)
    {
        int solidIdx = id - 1;
        m_doc->SetSelection({ SelectionType::WorldSolid, solidIdx, -1 });
        if (m_logFunc)
        {
            char buf[256];
            snprintf(buf, sizeof(buf), "Selected: brush (solid index %d)", solidIdx);
            m_logFunc(buf);
        }
    }
    else
    {
        int entIdx = id - worldCount - 1;
        if (entIdx >= 0 && entIdx < (int)m_doc->GetEntities().size())
        {
            m_doc->SetSelection({ SelectionType::Entity, entIdx, -1 });
            if (m_logFunc)
            {
                const auto& ent = m_doc->GetEntities()[entIdx];
                char buf[256];
                snprintf(buf, sizeof(buf), "Selected: %s (id %d)", ent.classname.c_str(), ent.id);
                m_logFunc(buf);
            }
        }
        else
        {
            m_doc->ClearSelection();
        }
    }
}

// ---- Object Movement ----

void ViewportPanel::HandleObjectMovement()
{
    if (!m_doc) return;

    ImGuiIO& io = ImGui::GetIO();
    bool hovered = ImGui::IsItemHovered();

    const auto& sel = m_doc->GetSelection();
    bool hasSelectedEntity = (sel.type == SelectionType::Entity &&
                              sel.index >= 0 && sel.index < (int)m_doc->GetEntities().size());
    bool hasSelectedBrush = (sel.type == SelectionType::WorldSolid &&
                             sel.index >= 0 && sel.index < (int)m_doc->GetWorld().solids.size());

    if (!hasSelectedEntity && !hasSelectedBrush)
    {
        m_dragging = false;
        return;
    }

    // === Entity movement ===
    if (hasSelectedEntity)
    {
        // Delete selected entity with Delete key
        if (hovered && ImGui::IsKeyPressed(ImGuiKey_Delete))
        {
            int idx = sel.index;
            if (m_logFunc)
            {
                const auto& ent = m_doc->GetEntities()[idx];
                char buf[256];
                snprintf(buf, sizeof(buf), "Deleted: %s (id %d)", ent.classname.c_str(), ent.id);
                m_logFunc(buf);
            }
            m_doc->RemoveEntity(idx);
            m_renderer.MarkDirty();
            if (m_sceneChanged) m_sceneChanged();
            return;
        }

        // Keyboard movement with Ctrl+Arrow keys
        if (hovered && !m_captured)
        {
            float moveStep = 16.0f;
            if (io.KeyShift) moveStep = 64.0f;

            Vec3 srcOrigin = m_doc->GetEntities()[sel.index].GetOrigin();
            bool moved = false;

            if (io.KeyCtrl)
            {
                if (ImGui::IsKeyPressed(ImGuiKey_UpArrow))    { srcOrigin.y += moveStep; moved = true; }
                if (ImGui::IsKeyPressed(ImGuiKey_DownArrow))  { srcOrigin.y -= moveStep; moved = true; }
                if (ImGui::IsKeyPressed(ImGuiKey_LeftArrow))  { srcOrigin.x -= moveStep; moved = true; }
                if (ImGui::IsKeyPressed(ImGuiKey_RightArrow)) { srcOrigin.x += moveStep; moved = true; }
                if (ImGui::IsKeyPressed(ImGuiKey_PageUp))     { srcOrigin.z += moveStep; moved = true; }
                if (ImGui::IsKeyPressed(ImGuiKey_PageDown))   { srcOrigin.z -= moveStep; moved = true; }
            }

            if (moved)
            {
                m_doc->MoveEntity(sel.index, srcOrigin);
                m_renderer.MarkDirty();
                if (m_sceneChanged) m_sceneChanged();
            }
        }

        // Middle mouse drag movement
        if (hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Middle))
        {
            m_dragging = true;
            m_dragStartPos = m_doc->GetEntities()[sel.index].GetOrigin();

            Vec3 glPos = SourceToGL(m_dragStartPos.x, m_dragStartPos.y, m_dragStartPos.z);
            Vec3 toEnt = glPos - m_camera.position;

            float yawRad   = DegToRad(m_camera.yaw);
            float pitchRad = DegToRad(m_camera.pitch);
            Vec3 forward;
            forward.x = cosf(pitchRad) * cosf(yawRad);
            forward.y = sinf(pitchRad);
            forward.z = cosf(pitchRad) * sinf(yawRad);

            m_dragDepth = Dot(toEnt, forward);
            if (m_dragDepth < 10.0f) m_dragDepth = 200.0f;
        }

        if (m_dragging && !ImGui::IsMouseDown(ImGuiMouseButton_Middle))
            m_dragging = false;

        if (m_dragging)
        {
            ImVec2 mousePos = io.MousePos;
            float localX = mousePos.x - m_vpMinX;
            float localY = mousePos.y - m_vpMinY;

            Vec3 rayDir = ScreenToWorldRay(localX, localY, m_fboWidth, m_fboHeight);
            Vec3 newGLPos = m_camera.position + rayDir * m_dragDepth;
            Vec3 newSrcPos = GLToSource(newGLPos);

            newSrcPos.x = floorf(newSrcPos.x / 16.0f + 0.5f) * 16.0f;
            newSrcPos.y = floorf(newSrcPos.y / 16.0f + 0.5f) * 16.0f;
            newSrcPos.z = floorf(newSrcPos.z / 16.0f + 0.5f) * 16.0f;

            m_doc->MoveEntity(sel.index, newSrcPos);
            m_renderer.MarkDirty();
            if (m_sceneChanged) m_sceneChanged();
        }
    }

    // === Brush movement ===
    if (hasSelectedBrush)
    {
        // Delete selected brush with Delete key
        if (hovered && ImGui::IsKeyPressed(ImGuiKey_Delete))
        {
            int idx = sel.index;
            if (m_logFunc)
            {
                char buf[256];
                snprintf(buf, sizeof(buf), "Deleted brush %d", idx);
                m_logFunc(buf);
            }
            m_doc->RemoveWorldSolid(idx);
            m_renderer.MarkDirty();
            if (m_sceneChanged) m_sceneChanged();
            return;
        }

        // Keyboard movement with Ctrl+Arrow keys
        if (hovered && !m_captured && io.KeyCtrl)
        {
            float moveStep = 16.0f;
            if (io.KeyShift) moveStep = 64.0f;

            Vec3 delta = { 0, 0, 0 };

            if (ImGui::IsKeyPressed(ImGuiKey_UpArrow))    delta.y = moveStep;
            if (ImGui::IsKeyPressed(ImGuiKey_DownArrow))  delta.y = -moveStep;
            if (ImGui::IsKeyPressed(ImGuiKey_LeftArrow))  delta.x = -moveStep;
            if (ImGui::IsKeyPressed(ImGuiKey_RightArrow)) delta.x = moveStep;
            if (ImGui::IsKeyPressed(ImGuiKey_PageUp))     delta.z = moveStep;
            if (ImGui::IsKeyPressed(ImGuiKey_PageDown))   delta.z = -moveStep;

            if (delta.x != 0 || delta.y != 0 || delta.z != 0)
            {
                m_doc->MoveWorldSolid(sel.index, delta);
                m_renderer.MarkDirty();
                if (m_sceneChanged) m_sceneChanged();
            }
        }

        // Middle mouse drag movement for brushes
        if (hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Middle))
        {
            m_dragging = true;
            // Store current center as drag start
            m_dragStartPos = m_doc->GetWorldSolidCenter(sel.index);

            Vec3 glPos = SourceToGL(m_dragStartPos.x, m_dragStartPos.y, m_dragStartPos.z);
            Vec3 toBrush = glPos - m_camera.position;

            float yawRad   = DegToRad(m_camera.yaw);
            float pitchRad = DegToRad(m_camera.pitch);
            Vec3 forward;
            forward.x = cosf(pitchRad) * cosf(yawRad);
            forward.y = sinf(pitchRad);
            forward.z = cosf(pitchRad) * sinf(yawRad);

            m_dragDepth = Dot(toBrush, forward);
            if (m_dragDepth < 10.0f) m_dragDepth = 200.0f;
        }

        if (m_dragging && !ImGui::IsMouseDown(ImGuiMouseButton_Middle))
            m_dragging = false;

        if (m_dragging)
        {
            ImVec2 mousePos = io.MousePos;
            float localX = mousePos.x - m_vpMinX;
            float localY = mousePos.y - m_vpMinY;

            Vec3 rayDir = ScreenToWorldRay(localX, localY, m_fboWidth, m_fboHeight);
            Vec3 newGLPos = m_camera.position + rayDir * m_dragDepth;
            Vec3 newSrcPos = GLToSource(newGLPos);

            // Snap to grid
            newSrcPos.x = floorf(newSrcPos.x / 16.0f + 0.5f) * 16.0f;
            newSrcPos.y = floorf(newSrcPos.y / 16.0f + 0.5f) * 16.0f;
            newSrcPos.z = floorf(newSrcPos.z / 16.0f + 0.5f) * 16.0f;

            // Calculate delta from original start position
            Vec3 delta = {
                newSrcPos.x - m_dragStartPos.x,
                newSrcPos.y - m_dragStartPos.y,
                newSrcPos.z - m_dragStartPos.z
            };

            // Only move if there's actual movement
            if (delta.x != 0 || delta.y != 0 || delta.z != 0)
            {
                m_doc->MoveWorldSolid(sel.index, delta);
                // Update drag start to current position to accumulate incremental movements
                m_dragStartPos = newSrcPos;
                m_renderer.MarkDirty();
                if (m_sceneChanged) m_sceneChanged();
            }
        }
    }
}

// ---- Brush Resize ----

void ViewportPanel::UpdateResizeHandles()
{
    if (!m_doc)
    {
        m_renderer.HideResizeHandles();
        return;
    }

    const auto& sel = m_doc->GetSelection();
    if (sel.type != SelectionType::WorldSolid || sel.index < 0)
    {
        m_renderer.HideResizeHandles();
        return;
    }

    const auto& solids = m_doc->GetWorld().solids;
    if (sel.index >= (int)solids.size())
    {
        m_renderer.HideResizeHandles();
        return;
    }

    // Compute bounds in GL space
    Vec3 mins, maxs;
    VmfDocument::ComputeSolidBoundsGL(solids[sel.index], mins, maxs);

    m_renderer.SetResizeHandles(mins, maxs);
}

int ViewportPanel::PickResizeHandle(const Vec3& rayOrigin, const Vec3& rayDir) const
{
    Vec3 centers[6];
    m_renderer.GetResizeHandleCenters(centers);

    float handleSize = 6.0f;  // Slightly larger for easier picking
    float bestT = 1e30f;
    int bestHandle = -1;

    for (int i = 0; i < 6; i++)
    {
        Vec3 hmins = centers[i] - Vec3{ handleSize, handleSize, handleSize };
        Vec3 hmaxs = centers[i] + Vec3{ handleSize, handleSize, handleSize };

        float t = 0;
        if (RayIntersectAABB(rayOrigin, rayDir, hmins, hmaxs, t))
        {
            if (t < bestT)
            {
                bestT = t;
                bestHandle = i;
            }
        }
    }

    return bestHandle;
}

void ViewportPanel::HandleBrushResize()
{
    if (!m_doc) return;

    const auto& sel = m_doc->GetSelection();
    if (sel.type != SelectionType::WorldSolid || sel.index < 0)
    {
        m_resizePhase = ResizePhase::None;
        return;
    }

    ImGuiIO& io = ImGui::GetIO();
    ImVec2 mousePos = io.MousePos;
    float localX = mousePos.x - m_vpMinX;
    float localY = mousePos.y - m_vpMinY;

    Vec3 rayDir = ScreenToWorldRay(localX, localY, m_fboWidth, m_fboHeight);
    Vec3 rayOrigin = m_camera.position;

    // Helper lambda to update resize preview
    auto updateResizePreview = [&]() {
        Vec3 newPos = rayOrigin + rayDir * m_resizeDragDepth;

        float gridSize = 16.0f;
        newPos.x = floorf(newPos.x / gridSize + 0.5f) * gridSize;
        newPos.y = floorf(newPos.y / gridSize + 0.5f) * gridSize;
        newPos.z = floorf(newPos.z / gridSize + 0.5f) * gridSize;

        m_resizeCurrentMins = m_resizeOriginalMins;
        m_resizeCurrentMaxs = m_resizeOriginalMaxs;

        switch (m_resizeFaceIndex)
        {
        case 0: m_resizeCurrentMaxs.x = fmaxf(newPos.x, m_resizeOriginalMins.x + gridSize); break; // +X
        case 1: m_resizeCurrentMins.x = fminf(newPos.x, m_resizeOriginalMaxs.x - gridSize); break; // -X
        case 2: m_resizeCurrentMaxs.y = fmaxf(newPos.y, m_resizeOriginalMins.y + gridSize); break; // +Y
        case 3: m_resizeCurrentMins.y = fminf(newPos.y, m_resizeOriginalMaxs.y - gridSize); break; // -Y
        case 4: m_resizeCurrentMaxs.z = fmaxf(newPos.z, m_resizeOriginalMins.z + gridSize); break; // +Z
        case 5: m_resizeCurrentMins.z = fminf(newPos.z, m_resizeOriginalMaxs.z - gridSize); break; // -Z
        }

        m_renderer.SetPreviewBox(m_resizeCurrentMins, m_resizeCurrentMaxs);
    };

    // Helper lambda to apply resize
    auto applyResize = [&]() {
        m_renderer.HidePreview();

        Vec3 srcMins = GLToSource(m_resizeCurrentMins);
        Vec3 srcMaxs = GLToSource(m_resizeCurrentMaxs);

        if (srcMins.x > srcMaxs.x) { float t = srcMins.x; srcMins.x = srcMaxs.x; srcMaxs.x = t; }
        if (srcMins.y > srcMaxs.y) { float t = srcMins.y; srcMins.y = srcMaxs.y; srcMaxs.y = t; }
        if (srcMins.z > srcMaxs.z) { float t = srcMins.z; srcMins.z = srcMaxs.z; srcMaxs.z = t; }

        m_doc->ResizeWorldSolid(sel.index, srcMins, srcMaxs);
        m_renderer.MarkDirty();
        if (m_sceneChanged)
            m_sceneChanged();

        if (m_logFunc)
        {
            char buf[256];
            snprintf(buf, sizeof(buf), "Resized brush to (%.0f, %.0f, %.0f) - (%.0f, %.0f, %.0f)",
                srcMins.x, srcMins.y, srcMins.z, srcMaxs.x, srcMaxs.y, srcMaxs.z);
            m_logFunc(buf);
        }

        m_resizePhase = ResizePhase::None;
    };

    if (m_resizePhase == ResizePhase::None)
    {
        if (ImGui::IsMouseClicked(ImGuiMouseButton_Left))
        {
            int handle = PickResizeHandle(rayOrigin, rayDir);
            if (handle >= 0)
            {
                m_resizePhase = ResizePhase::Dragging;
                m_resizeFaceIndex = handle;
                m_resizeWasDragged = false;

                // Store original bounds
                const auto& solids = m_doc->GetWorld().solids;
                VmfDocument::ComputeSolidBoundsGL(solids[sel.index], m_resizeOriginalMins, m_resizeOriginalMaxs);
                m_resizeCurrentMins = m_resizeOriginalMins;
                m_resizeCurrentMaxs = m_resizeOriginalMaxs;

                // Calculate depth for dragging
                Vec3 centers[6];
                m_renderer.GetResizeHandleCenters(centers);
                Vec3 toHandle = centers[handle] - m_camera.position;
                float yawRad = m_camera.yaw * (float)(M_PI / 180.0);
                float pitchRad = m_camera.pitch * (float)(M_PI / 180.0);
                Vec3 forward;
                forward.x = cosf(pitchRad) * cosf(yawRad);
                forward.y = sinf(pitchRad);
                forward.z = cosf(pitchRad) * sinf(yawRad);
                m_resizeDragDepth = Dot(toHandle, forward);
            }
        }
    }
    else if (m_resizePhase == ResizePhase::Dragging)
    {
        // Track if mouse is being dragged
        if (io.MouseDelta.x != 0 || io.MouseDelta.y != 0)
            m_resizeWasDragged = true;

        updateResizePreview();

        if (ImGui::IsMouseReleased(ImGuiMouseButton_Left))
        {
            if (m_resizeWasDragged)
            {
                // Normal drag behavior - apply immediately
                applyResize();
            }
            else
            {
                // Click without drag - switch to click mode
                m_resizePhase = ResizePhase::ClickMode;
            }
        }

        if (ImGui::IsMouseClicked(ImGuiMouseButton_Right) ||
            ImGui::IsKeyPressed(ImGuiKey_Escape))
        {
            m_renderer.HidePreview();
            m_resizePhase = ResizePhase::None;
        }
    }
    else if (m_resizePhase == ResizePhase::ClickMode)
    {
        // In click mode - follow mouse without holding button
        updateResizePreview();

        // Left click confirms
        if (ImGui::IsMouseClicked(ImGuiMouseButton_Left))
        {
            applyResize();
        }

        // Right click or Escape cancels
        if (ImGui::IsMouseClicked(ImGuiMouseButton_Right) ||
            ImGui::IsKeyPressed(ImGuiKey_Escape))
        {
            m_renderer.HidePreview();
            m_resizePhase = ResizePhase::None;
        }
    }
}

// ---- FBO Management ----

void ViewportPanel::CreateFBO(int width, int height)
{
    DestroyFBO();

    m_fboWidth  = width;
    m_fboHeight = height;

    m_sceneFB = m_rhi->CreateFramebuffer(width, height);
    m_pickFB  = m_rhi->CreateFramebuffer(width, height);
}

void ViewportPanel::DestroyFBO()
{
    if (m_sceneFB) { m_rhi->DestroyFramebuffer(m_sceneFB); m_sceneFB = RHI_NULL_FRAMEBUFFER; }
    if (m_pickFB)  { m_rhi->DestroyFramebuffer(m_pickFB);  m_pickFB  = RHI_NULL_FRAMEBUFFER; }
    m_fboWidth  = 0;
    m_fboHeight = 0;
}

void ViewportPanel::RenderScene()
{
    m_rhi->SetViewport(0, 0, m_fboWidth, m_fboHeight);
    m_rhi->Clear(0.15f, 0.15f, 0.15f, 1.0f);

    float aspect = (float)m_fboWidth / (float)m_fboHeight;
    Mat4 proj = Mat4::Perspective(70.0f, aspect, 1.0f, 50000.0f);

    float yawRad   = DegToRad(m_camera.yaw);
    float pitchRad = DegToRad(m_camera.pitch);

    Vec3 forward;
    forward.x = cosf(pitchRad) * cosf(yawRad);
    forward.y = sinf(pitchRad);
    forward.z = cosf(pitchRad) * sinf(yawRad);

    Vec3 eye = m_camera.position;
    Vec3 center = eye + forward;
    Vec3 up = { 0, 1, 0 };

    Mat4 view = Mat4::LookAt(eye, center, up);

    // Cache matrices for picking and gizmo
    m_lastView = view;
    m_lastProj = proj;

    // Pass selection info to renderer for highlight
    if (m_doc)
    {
        const auto& sel = m_doc->GetSelection();
        if (sel.type == SelectionType::Entity)
            m_renderer.SetSelectedEntityIndex(sel.index);
        else
            m_renderer.SetSelectedEntityIndex(-1);
    }

    m_renderer.Render(view, proj);
}

// ---- Möller–Trumbore ray-triangle intersection ----

bool ViewportPanel::RayIntersectTriangle(const Vec3& origin, const Vec3& dir,
                                          const Vec3& v0, const Vec3& v1, const Vec3& v2,
                                          float& t, Vec3& hitPos) const
{
    const float EPSILON = 1e-6f;
    Vec3 edge1 = v1 - v0;
    Vec3 edge2 = v2 - v0;
    Vec3 h = Cross(dir, edge2);
    float a = Dot(edge1, h);

    if (a > -EPSILON && a < EPSILON)
        return false;

    float f = 1.0f / a;
    Vec3 s = origin - v0;
    float u = f * Dot(s, h);
    if (u < 0.0f || u > 1.0f)
        return false;

    Vec3 q = Cross(s, edge1);
    float v = f * Dot(dir, q);
    if (v < 0.0f || u + v > 1.0f)
        return false;

    t = f * Dot(edge2, q);
    if (t < EPSILON)
        return false;

    hitPos = origin + dir * t;
    return true;
}

// ---- Ray pick brush face (for displacement creation) ----

bool ViewportPanel::RayPickBrushFace(const Vec3& rayOrigin, const Vec3& rayDir, RayFaceHit& hit) const
{
    if (!m_doc) return false;

    const auto& solids = m_doc->GetWorld().solids;
    float bestT = 1e30f;
    bool found = false;

    for (int si = 0; si < (int)solids.size(); si++)
    {
        const VmfSolid& solid = solids[si];
        BrushMesh mesh = BuildBrushMesh(solid);

        int faceIdx = 0;
        for (int sideIdx = 0; sideIdx < (int)solid.sides.size(); sideIdx++)
        {
            // Skip displacement faces - we want flat brush faces only
            if (solid.sides[sideIdx].dispinfo.has_value())
                continue;

            // Find the BrushFace(s) for this side by matching material
            // Since non-displacement faces are 1:1 with sides, we can iterate
            // the mesh faces and find matching ones
        }

        // Simpler approach: test all non-displacement mesh faces
        for (const auto& face : mesh.faces)
        {
            if (face.vertices.size() < 3) continue;

            // Fan-triangulate the face polygon
            for (size_t vi = 1; vi + 1 < face.vertices.size(); vi++)
            {
                float t;
                Vec3 hp;
                if (RayIntersectTriangle(rayOrigin, rayDir,
                    face.vertices[0].pos, face.vertices[vi].pos, face.vertices[vi + 1].pos,
                    t, hp))
                {
                    if (t < bestT)
                    {
                        bestT = t;
                        found = true;
                        hit.hitPos = hp;
                        hit.t = t;
                        hit.solidIndex = si;

                        // Find the side index by matching material
                        hit.sideIndex = 0;
                        for (int s = 0; s < (int)solid.sides.size(); s++)
                        {
                            if (solid.sides[s].material == face.material &&
                                !solid.sides[s].dispinfo.has_value())
                            {
                                hit.sideIndex = s;
                                break;
                            }
                        }
                    }
                }
            }
        }
    }

    return found;
}

// ---- Ray pick displacement (for sculpting) ----

bool ViewportPanel::RayPickDisplacement(const Vec3& rayOrigin, const Vec3& rayDir, RayDispHit& hit) const
{
    if (!m_doc) return false;

    const auto& solids = m_doc->GetWorld().solids;
    float bestT = 1e30f;
    bool found = false;

    for (int si = 0; si < (int)solids.size(); si++)
    {
        const VmfSolid& solid = solids[si];
        for (int sideIdx = 0; sideIdx < (int)solid.sides.size(); sideIdx++)
        {
            if (!solid.sides[sideIdx].dispinfo.has_value())
                continue;

            std::vector<Vec3> gridPos = ComputeDispGridPositions(solid, sideIdx);
            if (gridPos.empty())
                continue;

            int gridSize = solid.sides[sideIdx].dispinfo->GridSize();
            int cells = gridSize - 1;

            for (int row = 0; row < cells; row++)
            {
                for (int col = 0; col < cells; col++)
                {
                    int tl = row * gridSize + col;
                    int tr = row * gridSize + col + 1;
                    int bl = (row + 1) * gridSize + col;
                    int br = (row + 1) * gridSize + col + 1;

                    float t;
                    Vec3 hp;

                    // Triangle 1: tl, bl, tr
                    if (RayIntersectTriangle(rayOrigin, rayDir,
                        gridPos[tl], gridPos[bl], gridPos[tr], t, hp))
                    {
                        if (t < bestT)
                        {
                            bestT = t;
                            found = true;
                            hit.solidIndex = si;
                            hit.sideIndex = sideIdx;
                            hit.hitPos = hp;
                            hit.hitPosSource = GLToSource(hp);
                            hit.t = t;
                        }
                    }

                    // Triangle 2: tr, bl, br
                    if (RayIntersectTriangle(rayOrigin, rayDir,
                        gridPos[tr], gridPos[bl], gridPos[br], t, hp))
                    {
                        if (t < bestT)
                        {
                            bestT = t;
                            found = true;
                            hit.solidIndex = si;
                            hit.sideIndex = sideIdx;
                            hit.hitPos = hp;
                            hit.hitPosSource = GLToSource(hp);
                            hit.t = t;
                        }
                    }
                }
            }
        }
    }

    return found;
}

// ---- Displacement tool interaction ----

void ViewportPanel::HandleDisplacementTool()
{
    ImGuiIO& io = ImGui::GetIO();

    ImVec2 mousePos = io.MousePos;
    float localX = mousePos.x - m_vpMinX;
    float localY = mousePos.y - m_vpMinY;
    Vec3 rayDir = ScreenToWorldRay(localX, localY, m_fboWidth, m_fboHeight);
    Vec3 rayOrigin = m_camera.position;

    DispToolMode mode = m_dispTool->GetMode();

    if (mode == DispToolMode::Create)
    {
        // Create mode: click on a brush face to create a displacement
        m_dispBrushVisible = false;
        m_renderer.HideDispBrushCursor();

        if (ImGui::IsMouseClicked(ImGuiMouseButton_Left))
        {
            RayFaceHit faceHit;
            if (RayPickBrushFace(rayOrigin, rayDir, faceHit))
            {
                int power = m_dispTool->GetPower();
                if (m_doc->CreateDisplacement(faceHit.solidIndex, faceHit.sideIndex, power))
                {
                    m_renderer.MarkDirty();
                    if (m_sceneChanged) m_sceneChanged();
                    if (m_logFunc) m_logFunc("Created displacement surface.");
                }
            }
        }
    }
    else
    {
        // Sculpt / Smooth / Paint modes
        RayDispHit dispHit;
        if (RayPickDisplacement(rayOrigin, rayDir, dispHit))
        {
            m_dispBrushPos = dispHit.hitPos;
            m_dispBrushVisible = true;

            // Compute approximate surface normal at hit point
            const auto& solid = m_doc->GetWorld().solids[dispHit.solidIndex];
            const auto& side = solid.sides[dispHit.sideIndex];
            Vec3 e1 = SourceToGL(side.planePoints[1].x, side.planePoints[1].y, side.planePoints[1].z) -
                       SourceToGL(side.planePoints[0].x, side.planePoints[0].y, side.planePoints[0].z);
            Vec3 e2 = SourceToGL(side.planePoints[2].x, side.planePoints[2].y, side.planePoints[2].z) -
                       SourceToGL(side.planePoints[0].x, side.planePoints[0].y, side.planePoints[0].z);
            m_dispBrushNormal = Normalize(Cross(e1, e2));

            float brushRadius = m_dispTool->GetBrushSize();
            m_renderer.SetDispBrushCursor(m_dispBrushPos, m_dispBrushNormal, brushRadius);

            // Apply sculpt/smooth/paint while LMB is held
            if (ImGui::IsMouseDown(ImGuiMouseButton_Left))
            {
                VmfSolid& solidMut = m_doc->GetWorldMut().solids[dispHit.solidIndex];
                VmfSide& sideMut = solidMut.sides[dispHit.sideIndex];
                VmfDispInfo& disp = sideMut.dispinfo.value();
                int gridSize = disp.GridSize();

                float strength = m_dispTool->GetBrushStrength() * io.DeltaTime * 200.0f;
                float falloff = m_dispTool->GetBrushFalloff();
                bool invert = io.KeyShift;

                // Compute grid positions in Source space for distance comparison
                std::vector<Vec3> gridPosGL = ComputeDispGridPositions(solidMut, dispHit.sideIndex);

                for (int row = 0; row < gridSize; row++)
                {
                    for (int col = 0; col < gridSize; col++)
                    {
                        int idx = row * gridSize + col;
                        if (idx >= (int)gridPosGL.size()) continue;

                        Vec3 vertPosSource = GLToSource(gridPosGL[idx]);
                        Vec3 diff = vertPosSource - dispHit.hitPosSource;
                        float dist = Length(diff);

                        if (dist < brushRadius)
                        {
                            float weight = powf(1.0f - dist / brushRadius, falloff);

                            if (mode == DispToolMode::Sculpt)
                            {
                                disp.distances[idx] += weight * strength * (invert ? -1.0f : 1.0f);
                            }
                            else if (mode == DispToolMode::Smooth)
                            {
                                // Average neighbor distances
                                float avg = 0.0f;
                                int count = 0;
                                for (int dr = -1; dr <= 1; dr++)
                                {
                                    for (int dc = -1; dc <= 1; dc++)
                                    {
                                        int nr = row + dr;
                                        int nc = col + dc;
                                        if (nr >= 0 && nr < gridSize && nc >= 0 && nc < gridSize)
                                        {
                                            avg += disp.distances[nr * gridSize + nc];
                                            count++;
                                        }
                                    }
                                }
                                if (count > 0)
                                {
                                    avg /= (float)count;
                                    float blend = weight * strength * 0.1f;
                                    if (blend > 1.0f) blend = 1.0f;
                                    disp.distances[idx] += (avg - disp.distances[idx]) * blend;
                                }
                            }
                            else if (mode == DispToolMode::Paint)
                            {
                                float target = invert ? 0.0f : m_dispTool->GetPaintAlpha();
                                float blend = weight * strength * 0.1f;
                                if (blend > 1.0f) blend = 1.0f;
                                disp.alphas[idx] += (target - disp.alphas[idx]) * blend;
                                if (disp.alphas[idx] < 0.0f) disp.alphas[idx] = 0.0f;
                                if (disp.alphas[idx] > 255.0f) disp.alphas[idx] = 255.0f;
                            }
                        }
                    }
                }

                m_renderer.MarkDirty();
                if (m_sceneChanged) m_sceneChanged();
                m_dispSculpting = true;
            }
            else
            {
                m_dispSculpting = false;
            }
        }
        else
        {
            m_dispBrushVisible = false;
            m_renderer.HideDispBrushCursor();
            m_dispSculpting = false;
        }
    }
}
