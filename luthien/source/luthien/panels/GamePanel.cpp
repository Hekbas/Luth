#include "lepch.h"
#include "luthien/panels/GamePanel.h"

#include "luthien/EditorSnapshot.h"
#include "luthien/EditorSettings.h"
#include "luthien/widgets/Icons.h"
#include "luthien/panels/FrameDebuggerPanel.h"
#include "luth/scene/Scene.h"
#include "luth/scene/Components.h"
#include "luth/scene/systems/RenderingSystem.h"
#include "luth/renderer/RenderPipeline.h"
#include "luth/renderer/Renderer.h"
#include "luth/renderer/RenderBackend.h"

#include <imgui.h>

namespace Luth
{
    using namespace Component;

    GamePanel::GamePanel(RenderingSystem* renderingSystem)
        : m_RenderingSystem(renderingSystem)
        , m_Viewport(std::make_unique<ViewportRenderer>())
    {
        m_WindowID = "Game";
        m_Viewport->SetOnResize([this](u32 w, u32 h) {
            // Drain GPU + drop ViewResources before swapping FrameTargets;
            // the size-keyed cache otherwise leaves descriptors pointing at
            // views the deletion queue is about to destroy.
            Renderer::WaitForGPU();
            m_RenderingSystem->GetPipeline().ReleaseViewResources(m_Targets);

            if (!m_TargetsAllocated) {
                m_Targets.Allocate(w, h);
                m_TargetsAllocated = true;
            } else {
                m_Targets.Resize(w, h);
            }
            m_Viewport->SetSize(w, h);
        });

        LH_CORE_INFO("Created Game panel");
    }

    GamePanel::~GamePanel() = default;

    void GamePanel::OnInit() {}

    // Populate CameraParams + aspect ratio from the first
    // <Camera, WorldTransform> entity. Returns false if none.
    //
    // Projection stays Y-up — RenderPipeline::UpdateGlobalUniforms applies
    // the Vulkan Y-flip uniformly for every view. IBL/skybox intensities
    // read from EditorSettings so both views share the Render panel sliders.
    static bool BuildCameraFromScene(entt::registry& reg, CameraParams& out, float& outAspect)
    {
        auto view = reg.view<Camera, WorldTransform>();
        auto it = view.begin();
        if (it == view.end()) return false;

        auto& cam = view.get<Camera>(*it);
        auto& xf  = view.get<WorldTransform>(*it);

        out.view = Math::Inverse(xf.Matrix);
        if (cam.Projection == Camera::ProjectionType::Perspective) {
            out.projection = Math::Perspective(
                Math::Radians(cam.VerticalFOV), cam.AspectRatio,
                cam.NearClip, cam.FarClip);
            out.nearZ = cam.NearClip;
            out.farZ  = cam.FarClip;
        } else {
            const float l = -cam.OrthographicSize * cam.AspectRatio * 0.5f;
            const float r =  cam.OrthographicSize * cam.AspectRatio * 0.5f;
            const float b = -cam.OrthographicSize * 0.5f;
            const float t =  cam.OrthographicSize * 0.5f;
            out.projection = Math::Ortho(l, r, b, t,
                cam.OrthographicNear, cam.OrthographicFar);
            out.nearZ = cam.OrthographicNear;
            out.farZ  = cam.OrthographicFar;
        }
        out.position        = Vec3(xf.Matrix[3][0], xf.Matrix[3][1], xf.Matrix[3][2]);
        out.iblIntensity    = Editor::GetSettings().iblIntensity;
        out.skyboxIntensity = Editor::GetSettings().skyboxIntensity;
        outAspect           = cam.AspectRatio;
        return true;
    }

    void GamePanel::OnGather(EditorSnapshotBuilder& builder)
    {
        // No state to capture beyond the placeholder fragment — Game viewport is
        // fully ImGui-driven against the live RenderingSystem game-camera target.
        builder.Add<GameViewportSnapshot>();
    }

    void GamePanel::OnDraw(const EditorSnapshot& /*snapshot*/)
    {
        LH_PROFILE_FUNCTION();
        ImGui::PushFont(Editor::GetFASolid());
        std::string title = ICON_FA_GAMEPAD + std::string("  Game");

        if (BeginWindow(title.c_str(), ImGuiWindowFlags_NoScrollbar))
        {
            ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));

            auto scene = Editor::GetActiveScene();
            const bool haveBackend = Renderer::GetBackend() && Renderer::GetBackend()->GetAPI() == RenderBackend::API::Vulkan;

            // Resolve the camera before BeginViewport so the viewport can
            // lock to its aspect (letterbox/pillarbox). No camera → free
            // aspect fallback for the placeholder text.
            CameraParams camera;
            float camAspect = 0.0f;
            const bool haveCamera = scene && haveBackend
                                        && BuildCameraFromScene(scene->Registry(), camera, camAspect);

            m_Viewport->BeginViewport(camAspect);

            if (haveCamera && m_TargetsAllocated) {
                RenderView gameView;
                gameView.targets              = &m_Targets;
                gameView.camera               = camera;
                gameView.viewIndex            = 1;
                gameView.drawGrid             = false;
                gameView.drawSelectionOutline = false;
                gameView.emitImGuiPass        = false;
                // Frame Debugger capture source: route the sink to this view's
                // RG when the user selected Game. Single-view capture model.
                gameView.captureRequested     = (m_RenderingSystem->GetDebuggerState() == DebuggerState::CaptureRequested
                                                 && m_RenderingSystem->GetCaptureSource() == CaptureSource::Game);
                m_RenderingSystem->QueueView(gameView);

                // Frame Debugger viewport overlay (Unity-style): when Frozen
                // and the panel is set to overlay in the Game viewport, render
                // the selected pass's archived RT instead of the live LDR.
                FrameDebuggerPanel::OverlaySource fdOverlay{};
                if (auto* fd = Editor::GetPanel<FrameDebuggerPanel>(); fd && fd->ShouldOverlayInGame())
                    fdOverlay = fd->GetOverlaySource();

                if (fdOverlay.view != VK_NULL_HANDLE) {
                    m_Viewport->DrawSceneTextureRaw(fdOverlay.view, fdOverlay.sampler);
                } else {
                    const auto& ldr = m_Targets.GetLDROutput();
                    m_Viewport->DrawSceneTexture(ldr ? ldr : m_Targets.GetSceneColor());
                }
            } else {
                const ImVec2 avail = ImGui::GetContentRegionAvail();
                const char* msg = scene
                    ? "No Camera entity in scene"
                    : "No active scene";
                const ImVec2 textSize = ImGui::CalcTextSize(msg);
                const ImVec2 cursor = ImGui::GetCursorPos();
                ImGui::SetCursorPos({
                    cursor.x + (avail.x - textSize.x) * 0.5f,
                    cursor.y + (avail.y - textSize.y) * 0.5f });
                ImGui::TextUnformatted(msg);
            }

            ImGui::PopStyleVar();
        }
        ImGui::End();
        ImGui::PopFont();
    }
}
