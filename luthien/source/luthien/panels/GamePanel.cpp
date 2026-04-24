#include "lepch.h"
#include "luthien/panels/GamePanel.h"

#include "luthien/EditorSettings.h"
#include "luthien/widgets/Icons.h"
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
        m_Viewport->SetOnResize([this](u32 w, u32 h) {
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

    // Build CameraParams + expose the camera's aspect ratio from the first
    // <Camera, WorldTransform> entity. Returns false if no such entity
    // exists (GamePanel shows a placeholder).
    //
    // NOTE: we leave projection Y-up (Math::Perspective / Math::Ortho output).
    // RenderPipeline::UpdateGlobalUniforms applies the Vulkan Y-flip uniformly
    // for every view — matching EditorCamera's convention. Pre-flipping here
    // double-flips and inverts triangle winding (upside-down + backfaces).
    //
    // IBL + skybox intensities come from EditorSettings (same source the
    // scene view's CameraParams uses via EditorHooks), so both views stay
    // in sync when the user tweaks those sliders in the Render panel.
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

    void GamePanel::OnRender()
    {
        ImGui::PushFont(Editor::GetFASolid());
        std::string title = ICON_FA_GAMEPAD + std::string("  Game");

        if (ImGui::Begin(title.c_str(), nullptr, ImGuiWindowFlags_NoScrollbar))
        {
            ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));

            auto scene = Editor::GetActiveScene();
            const bool haveBackend = Renderer::GetBackend() && Renderer::GetBackend()->GetAPI() == RenderBackend::API::Vulkan;

            // Look up the scene camera first so BeginViewport can lock the
            // inner rect to its aspect ratio (letterbox / pillarbox). No
            // camera → free aspect fallback so the placeholder text uses
            // the full panel area.
            CameraParams camera;
            float camAspect = 0.0f;
            const bool haveCamera = scene && haveBackend
                                        && BuildCameraFromScene(scene->Registry(), camera, camAspect);

            m_Viewport->BeginViewport(camAspect);

            if (haveCamera && m_TargetsAllocated) {
                // Queue this view for rendering in RS::Update. The scene
                // view's ImGui pass (which finalizes the frame after every
                // view's subgraph has recorded into the same primary cmd
                // buffer) will sample this LDR via ImGui::Image below.
                RenderView gameView;
                gameView.targets              = &m_Targets;
                gameView.camera               = camera;
                gameView.viewIndex            = 1;
                gameView.drawGrid             = false;
                gameView.drawSelectionOutline = false;
                gameView.emitImGuiPass        = false;
                m_RenderingSystem->QueueView(gameView);

                const auto& ldr = m_Targets.GetLDROutput();
                m_Viewport->DrawSceneTexture(ldr ? ldr : m_Targets.GetSceneColor());
            } else {
                // Center placeholder in the viewport region
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
