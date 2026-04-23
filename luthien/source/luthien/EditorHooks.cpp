#include "lepch.h"
#include "luthien/Bootstrap.h"
#include "luth/core/EditorHooks.h"
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
