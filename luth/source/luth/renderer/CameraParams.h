#pragma once

#include "luth/scene/Entity.h"

#include <vector>

namespace Luth
{
    // Camera + editor state needed by RenderingSystem each frame. Despite the
    // name, this carries all per-frame editor inputs (IBL/skybox intensity,
    // selection, outline + grid params) so the engine doesn't depend on luthien/.
    // Populated by App before SystemRegistry::Update<RenderingSystem>().
    struct CameraParams
    {
        Mat4 view       = Mat4(1.0f);
        Mat4 projection = Mat4(1.0f);
        Vec3 position   = Vec3(0.0f);
        float nearZ          = 0.1f;
        float farZ           = 1000.0f;
        float iblIntensity     = 1.0f;
        float skyboxIntensity  = 1.0f;
        bool  enableVolumetricFog = true;
        std::vector<Entity> selectedEntities;

        // Selection-outline + editor-grid params, plumbed through EditorSettings.
        // Runtime-only build leaves defaults intact.
        Vec4 outlineColor          = { 1.0f, 0.6f, 0.0f, 1.0f };
        float outlineWidth          = 1.5f;
        float outlineOccludedAlpha  = 0.65f;

        Vec4 gridAxisXColor    = { 0.80f, 0.10f, 0.15f, 1.00f };
        Vec4 gridAxisZColor    = { 0.10f, 0.25f, 0.80f, 1.00f };
        Vec4 gridColor         = { 0.41f, 0.41f, 0.41f, 0.50f };
        float gridMajorScale    = 1.0f;
        float gridFadeStart     = 20.0f;
        float gridFadeEnd       = 200.0f;
        float gridLineThickness = 1.0f;

        // Editor in-world gizmo scopes + palette. Engine defaults OFF so a runtime build draws nothing;
        // the editor flips them per EditorSettings. Light wireframes use the light's own color, not the
        // palette. Scope: draw when (all || selected&&isSel); All dims unselected to gizmoAlphaUnselected.
        bool lightsSelected  = false;  bool lightsAll  = false;
        bool camerasSelected = false;  bool camerasAll = false;
        bool boundsSelected  = false;  bool boundsAll  = false;
        bool bonesSelected   = false;  bool bonesAll   = false;
        bool fogSelected     = false;  bool fogAll     = false;
        bool windSelected    = false;  bool windAll    = false;
        float gizmoAlphaUnselected  = 0.5f;
        Vec4  gizmoCameraColor      = { 0.706f, 0.706f, 0.863f, 0.706f };
        Vec4  gizmoAABBColor        = { 0.627f, 0.627f, 0.627f, 0.392f };
        Vec4  gizmoAABBSelectedColor= { 1.000f, 0.627f, 0.000f, 0.784f };
        Vec4  gizmoBoneLineColor    = { 0.000f, 1.000f, 0.502f, 0.784f };
        Vec4  gizmoBoneJointColor   = { 1.000f, 1.000f, 0.000f, 1.000f };
        Vec4  gizmoFogColor         = { 0.300f, 0.800f, 1.000f, 0.500f };
        Vec4  gizmoWindColor        = { 0.600f, 0.900f, 1.000f, 0.800f };
    };
}
