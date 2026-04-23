#include "lepch.h"
#include "luthien/viewport/GizmoController.h"

#include "luthien/CommandHistory.h"
#include "luthien/commands/Commands.h"
#include "luth/scene/Components.h"

#include <ImGuizmo.h>

namespace Luth
{
    using namespace Component;

    void GizmoController::ResetFrameState()
    {
        m_IconClicked = false;
        m_IconEntity  = entt::null;
    }

    void GizmoController::DrawManipulator(const Mat4& view, const Mat4& proj,
                                          const ImVec2* bounds, const Vec2& size,
                                          Entity& selected, Scene* scene,
                                          bool isFocused, bool cameraFlying)
    {
        if (!selected || !selected.IsValid()) return;

        ImGuizmo::SetOrthographic(false);
        ImGuizmo::SetDrawlist();
        ImGuizmo::SetRect(bounds[0].x, bounds[0].y, size.x, size.y);

        auto& tc = selected.GetComponent<Transform>();
        Mat4 worldMatrix = selected.GetComponent<WorldTransform>().Matrix;

        // If dirty, reconstruct world matrix on the fly to be responsive
        if (tc.IsDirty)
        {
             // We can't easily reconstruct world matrix here without parent info.
             // Rely on the System to have updated it, OR force a quick calc if needed.
             // For now, let's trust the system update loop which runs before Render.
        }

        // Only draw & interact with the manipulator when visible and a tool is active
        if (m_ShowTransformGizmo && m_Operation != -1)
        {
            // Snapping
            bool snap = ImGui::IsKeyDown(ImGuiKey_LeftCtrl);
            float snapValue = 0.5f; // Snap to 0.5m for translation/scale
            if (m_Operation == ImGuizmo::OPERATION::ROTATE)
                snapValue = 45.0f; // Snap to 45 degrees for rotation

            float snapValues[3] = { snapValue, snapValue, snapValue };

            ImGuizmo::Manipulate(Math::ValuePtr(view), Math::ValuePtr(proj),
                (ImGuizmo::OPERATION)m_Operation, ImGuizmo::LOCAL, Math::ValuePtr(worldMatrix),
                nullptr, snap ? snapValues : nullptr);

            bool isUsing = ImGuizmo::IsUsing();

            // Capture transform at drag start
            if (isUsing && !m_WasUsing) {
                m_StartPos   = tc.Position;
                m_StartRot   = tc.Rotation;
                m_StartScale = tc.Scale;
            }

            if (isUsing)
            {
                // Convert back to Local Space
                Mat4 localMatrix = worldMatrix;
                if (selected.HasParent())
                {
                    Entity parent = selected.GetParent();
                    Mat4 parentWorld = parent.GetComponent<WorldTransform>().Matrix;
                    localMatrix = Math::Inverse(parentWorld) * worldMatrix;
                }

                float translation[3], rotation[3], scale[3];
                ImGuizmo::DecomposeMatrixToComponents(Math::ValuePtr(localMatrix), translation, rotation, scale);

                tc.Position = Math::MakeVec3(translation);
                tc.Rotation = Math::MakeVec3(rotation);
                tc.Scale = Math::MakeVec3(scale);
                tc.IsDirty = true;
            }

            // Push command at drag end
            if (!isUsing && m_WasUsing) {
                CommandHistory::Execute(std::make_unique<GizmoTransformCommand>(
                    scene, (entt::entity)selected,
                    m_StartPos, m_StartRot, m_StartScale,
                    tc.Position, tc.Rotation, tc.Scale));
            }

            m_WasUsing = isUsing;
        }

        // Gizmo Shortcuts
        if (isFocused && !ImGuizmo::IsUsing() && !cameraFlying)
        {
            if (ImGui::IsKeyPressed(ImGuiKey_Q))
                m_Operation = -1;
            if (ImGui::IsKeyPressed(ImGuiKey_W))
                m_Operation = ImGuizmo::OPERATION::TRANSLATE;
            if (ImGui::IsKeyPressed(ImGuiKey_E))
                m_Operation = ImGuizmo::OPERATION::ROTATE;
            if (ImGui::IsKeyPressed(ImGuiKey_R))
                m_Operation = ImGuizmo::OPERATION::SCALE;
        }
    }

    void GizmoController::DrawGizmoIcon(ImDrawList* drawList, ImVec2 screenPos, const char* icon,
                                        ImU32 color, entt::entity entity,
                                        bool isHovered, bool hasValidSelection)
    {
        constexpr float hitRadius = 16.0f;

        ImVec2 textSize = ImGui::CalcTextSize(icon);
        ImVec2 textPos = { screenPos.x - textSize.x * 0.5f, screenPos.y - textSize.y * 0.5f };
        drawList->AddText(textPos, color, icon);

        // Only consider ImGuizmo::IsOver() when a transform gizmo is actually active —
        // otherwise it returns stale state from the previous frame
        bool gizmoActive = hasValidSelection && m_ShowTransformGizmo && m_Operation != -1;

        // Hit-test for click-to-select
        if (isHovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left)
            && !(gizmoActive && ImGuizmo::IsOver())
            && !ImGui::IsKeyDown(ImGuiKey_LeftAlt) && !ImGui::IsKeyDown(ImGuiKey_RightAlt))
        {
            ImVec2 mouse = ImGui::GetMousePos();
            float dx = mouse.x - screenPos.x, dy = mouse.y - screenPos.y;
            if (dx * dx + dy * dy <= hitRadius * hitRadius) {
                m_IconClicked = true;
                m_IconEntity = entity;
            }
        }
    }
}
