#pragma once

#include "luth/core/types/LuthMath.h"
#include "luth/scene/Entity.h"

#include <imgui.h>
#include <entt/entt.hpp>

namespace Luth
{
    class Scene;

    // ImGuizmo wrapper. Holds the active gizmo operation (translate / rotate / scale) and the
    // 2D icon-overlay state for camera and light entities drawn over the viewport. ScenePanel
    // invokes it during its OnDraw — gizmo input is dominantly ImGui-driven so it stays inline.
    class GizmoController
    {
    public:
        int  GetOperation() const        { return m_Operation; }
        void SetOperation(int op)        { m_Operation = op; }

        bool IsTransformGizmoVisible() const           { return m_ShowTransformGizmo; }
        void SetTransformGizmoVisible(bool v)          { m_ShowTransformGizmo = v; }
        bool* GetTransformGizmoVisibleRef()            { return &m_ShowTransformGizmo; }

        // Called once per frame before drawing; clears last frame's icon-click latch.
        void ResetFrameState();

        // Runs ImGuizmo manipulator + drag-undo coalesce + Q/W/E/R shortcuts.
        // Early-returns when selected is null/invalid (matches pre-refactor behavior).
        void DrawManipulator(const Mat4& view, const Mat4& proj,
                             const ImVec2* bounds, const Vec2& size,
                             Entity& selected, Scene* scene,
                             bool isFocused, bool cameraFlying);

        // Renders a world-placed icon + hit-tests click-to-select for that entity.
        // hasValidSelection gates the ImGuizmo::IsOver() guard (stale otherwise).
        void DrawGizmoIcon(ImDrawList* drawList, ImVec2 screenPos, const char* icon,
                           ImU32 color, entt::entity entity,
                           bool isHovered, bool hasValidSelection);

        bool         WasIconClicked() const { return m_IconClicked; }
        entt::entity IconEntity()     const { return m_IconEntity; }

    private:
        int  m_Operation = -1;
        bool m_ShowTransformGizmo = true;

        // Captured on drag start so the whole drag coalesces into one undo entry.
        bool m_WasUsing = false;
        Vec3 m_StartPos{};
        Vec3 m_StartRot{};
        Vec3 m_StartScale{};

        // Icon click wins over pick result when both fire in the same frame.
        bool         m_IconClicked = false;
        entt::entity m_IconEntity  = entt::null;
    };
}
