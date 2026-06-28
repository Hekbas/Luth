#include "lepch.h"
#include "luthien/Bootstrap.h"
#include "luth/core/EditorHooks.h"
#include "luth/core/types/LuthMath.h"
#include "luth/core/diagnostics/Log.h"
#include "luth/scene/Components.h"
#include "luth/scene/Scene.h"
#include "luthien/Editor.h"
#include "luthien/EditorCamera.h"
#include "luthien/EditorColors.h"
#include "luthien/EditorSelection.h"
#include "luthien/PlayModeController.h"
#include "luthien/ProjectLauncher.h"
#include "luthien/panels/ScenePanel.h"
#include "luthien/panels/ProjectPanel.h"

namespace Luth
{
namespace
{
    class LuthienEditorHooks : public IEditorHooks
    {
    public:
        // Lifecycle
        void Init(Window* w) override
        {
            Editor::Init(w);
            ProjectLauncher::Init();
        }
        void SetActiveScene(std::shared_ptr<Scene> s) override { Editor::SetActiveScene(s); }
        void Shutdown() override                               { Editor::Shutdown(); }

        // Per-frame
        void BeginFrame() override { Editor::BeginFrame(); }
        void Render() override     { Editor::Render(); }
        void EndFrame() override   { Editor::EndFrame(); }

        // Project lifecycle
        void OnProjectChanged() override { Editor::OnProjectChanged(); }
        void SaveSettings() override     { Editor::SaveSettings(); }

        // Input capture
        bool WantCaptureKeyboard() override { return Editor::WantCaptureKeyboard(); }
        bool WantCaptureMouse() override    { return Editor::WantCaptureMouse(); }

        // Viewport state snapshot
        void GetViewportState(EditorViewportState& out) override
        {
            if (auto* sp = Editor::GetPanel<ScenePanel>())
            {
                EditorCamera& cam = sp->GetEditorCamera();
                out.hasCamera  = true;
                out.view       = cam.GetViewMatrix();
                out.projection = cam.GetProjectionMatrix();
                out.position   = cam.GetPosition();
                out.nearZ      = cam.GetNearClip();
                out.farZ       = cam.GetFarClip();
            }
            const auto& s = Editor::GetSettings();
            out.iblIntensity     = s.iblIntensity;
            out.skyboxIntensity  = s.skyboxIntensity;
            out.enableVolumetricFog = s.enableVolumetricFog;
            out.selectedEntities = EditorSelection::GetSelectedEntities();

            out.outlineColor          = s.outlineColor;
            out.outlineWidth          = s.outlineWidth;
            out.outlineOccludedAlpha  = s.outlineOccludedAlpha;

            out.gridAxisXColor    = s.gridAxisXColor;
            out.gridAxisZColor    = s.gridAxisZColor;
            out.gridColor         = s.gridColor;
            out.gridMajorScale    = s.gridMajorScale;
            out.gridFadeStart     = s.gridFadeStart;
            out.gridFadeEnd       = s.gridFadeEnd;
            out.gridLineThickness = s.gridLineThickness;

            // Gizmo toggles (settings) + palette (EditorColors, unpacked IM_COL32 → linear Vec4).
            auto colToVec4 = [](ImU32 c) {
                return Vec4(((c >> IM_COL32_R_SHIFT) & 0xFFu) / 255.0f,
                            ((c >> IM_COL32_G_SHIFT) & 0xFFu) / 255.0f,
                            ((c >> IM_COL32_B_SHIFT) & 0xFFu) / 255.0f,
                            ((c >> IM_COL32_A_SHIFT) & 0xFFu) / 255.0f);
            };
            out.showBoneDebug          = s.showBoneDebug;
            out.showLightGizmos        = s.showLightGizmos;
            out.showCameraGizmos       = s.showCameraGizmos;
            out.showAABBGizmos         = s.showAABBGizmos;
            out.gizmoCameraColor       = colToVec4(EditorColors::GizmoCamera);
            out.gizmoAABBColor         = colToVec4(EditorColors::GizmoAABB);
            out.gizmoAABBSelectedColor = colToVec4(EditorColors::GizmoAABBSelected);
            out.gizmoBoneLineColor     = colToVec4(EditorColors::GizmoBoneLine);
            out.gizmoBoneJointColor    = colToVec4(EditorColors::GizmoBoneJoint);

            out.previewAnimationInEditor = s.previewAnimationInEditor;

            out.physicsShapesSelected   = s.physicsShapesSelected;
            out.physicsShapesAll        = s.physicsShapesAll;
            out.physicsAABBsSelected    = s.physicsAABBsSelected;
            out.physicsAABBsAll         = s.physicsAABBsAll;
            out.physicsCoMSelected      = s.physicsCoMSelected;
            out.physicsCoMAll           = s.physicsCoMAll;
            out.physicsColorMode        = s.physicsColorMode;
            out.physicsUniformColor     = s.physicsUniformColor;
            out.physicsDebugSegments    = s.physicsDebugSegments;
            out.physicsAlphaUnselected  = s.physicsAlphaUnselected;
        }

        // Play-mode state forwarded from PlayModeController
        PlayState GetPlayState() const override { return PlayModeController::GetState(); }
        bool ConsumeStepRequest() override      { return PlayModeController::ConsumeStepRequest(); }

        std::filesystem::path GetProjectCurrentDir() override
        {
            if (auto* pp = Editor::GetPanel<ProjectPanel>())
                return pp->GetCurrentDirectory();
            return {};
        }

        // Surface engine notices in the console panel; future work can route to a
        // floating toast widget without touching the engine call site.
        void OnFrameDebuggerNotice(const std::string& message) override
        {
            LH_CORE_INFO("[FrameDebugger] {}", message);
        }

        // Project launcher
        void ShowProjectLauncher() override                    { Editor::ShowProjectLauncher(); }
        bool HasPendingProject() override                      { return ProjectLauncher::HasPendingProject(); }
        std::filesystem::path ConsumePendingProject() override { return ProjectLauncher::ConsumePendingProject(); }
        void AddRecentProject(const std::string& n, const std::filesystem::path& p) override { ProjectLauncher::AddRecent(n, p); }
        void HideProjectLauncher() override                    { ProjectLauncher::Hide(); }
        void SetPendingProject(const std::filesystem::path& p) override { ProjectLauncher::SetPendingProject(p); }
    };

    LuthienEditorHooks s_EditorHooks;
}

    void InstallLuthienEditorHooks()
    {
        EditorHooks::Register(&s_EditorHooks);
    }
}
