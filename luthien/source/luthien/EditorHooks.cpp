#include "lepch.h"
#include "luthien/Bootstrap.h"
#include "luth/core/EditorHooks.h"
#include "luth/core/types/LuthMath.h"
#include "luth/scene/Components.h"
#include "luth/scene/Scene.h"
#include "luthien/Editor.h"
#include "luthien/EditorCamera.h"
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
            out.iblIntensity     = Editor::GetSettings().iblIntensity;
            out.skyboxIntensity  = Editor::GetSettings().skyboxIntensity;
            out.selectedEntities = EditorSelection::GetSelectedEntities();
            out.previewAnimationInEditor = Editor::GetSettings().previewAnimationInEditor;

            // Scene-camera override during Playing/Paused (first Camera entity)
            const PlayState ps = PlayModeController::GetState();
            if (ps != PlayState::Editing && !Editor::GetSettings().useEditorCameraInPlay)
            {
                if (auto scene = Editor::GetActiveScene())
                {
                    auto view = scene->Registry().view<Component::Camera, Component::WorldTransform>();
                    auto it = view.begin();
                    if (it != view.end())
                    {
                        auto& cam = view.get<Component::Camera>(*it);
                        auto& xf  = view.get<Component::WorldTransform>(*it);

                        out.playView = Math::Inverse(xf.Matrix);
                        if (cam.Projection == Component::Camera::ProjectionType::Perspective) {
                            out.playProjection = Math::Perspective(
                                Math::Radians(cam.VerticalFOV), cam.AspectRatio,
                                cam.NearClip, cam.FarClip);
                        } else {
                            const float l = -cam.OrthographicSize * cam.AspectRatio * 0.5f;
                            const float r =  cam.OrthographicSize * cam.AspectRatio * 0.5f;
                            const float b = -cam.OrthographicSize * 0.5f;
                            const float t =  cam.OrthographicSize * 0.5f;
                            out.playProjection = Math::Ortho(l, r, b, t,
                                cam.OrthographicNear, cam.OrthographicFar);
                        }
                        out.playProjection[1][1] *= -1.0f; // Vulkan Y-flip
                        out.playPosition = Vec3(xf.Matrix[3][0], xf.Matrix[3][1], xf.Matrix[3][2]);
                        out.hasPlayCamera = true;
                    }
                    else
                    {
                        static bool s_WarnedNoPlayCam = false;
                        if (!s_WarnedNoPlayCam) {
                            LH_CORE_WARN("Play mode: no Camera entity in scene — using editor camera");
                            s_WarnedNoPlayCam = true;
                        }
                    }
                }
            }
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
