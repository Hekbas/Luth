#pragma once

namespace Luth
{
    class Scene;
    struct CameraParams;
    struct WindSettings;

    namespace Gizmos
    {
        // Engine-side editor-gizmo producer. Runs in the GAME stage (App::GameStageFn, after
        // AnimationSystem) so it shares DebugDraw's single-writer slot with PhysicsSystem and reads
        // fresh WorldTransform / bone / AABB data. Per-category gates live on cam (all false in a
        // runtime build → cheap no-op). World-space lines are flushed by DebugDrawSubsystem in the
        // scene view only; the 2D selection icons stay in the editor's ViewportOverlays. wind is the
        // global field (for Wind arrows whose entity doesn't override the direction).
        void Draw(Scene& scene, const CameraParams& cam, const WindSettings& wind);
    }
}
