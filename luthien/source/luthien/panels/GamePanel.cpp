#include "lepch.h"
#include "luthien/panels/GamePanel.h"

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

    // Build CameraParams from the first <Camera, WorldTransform> entity. Lifted
    // from the v2.8.0 EditorHooks scene-camera override path removed in C1.
    static bool BuildCameraFromScene(entt::registry& reg, CameraParams& out)
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
        out.projection[1][1] *= -1.0f; // Vulkan Y-flip
        out.position = Vec3(xf.Matrix[3][0], xf.Matrix[3][1], xf.Matrix[3][2]);
        return true;
    }

    void GamePanel::OnRender()
    {
        ImGui::PushFont(Editor::GetFASolid());
        std::string title = ICON_FA_GAMEPAD + std::string("  Game");

        if (ImGui::Begin(title.c_str(), nullptr, ImGuiWindowFlags_NoScrollbar))
        {
            ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));

            m_Viewport->BeginViewport();

            auto scene = Editor::GetActiveScene();
            const bool haveBackend = Renderer::GetBackend() && Renderer::GetBackend()->GetAPI() == RenderBackend::API::Vulkan;

            CameraParams camera;
            const bool haveCamera = scene && haveBackend && m_TargetsAllocated
                                        && BuildCameraFromScene(scene->Registry(), camera);

            if (haveCamera) {
                RenderView gameView;
                gameView.targets              = &m_Targets;
                gameView.camera               = camera;
                gameView.drawGrid             = false;
                gameView.drawSelectionOutline = false;
                m_RenderingSystem->RenderToView(gameView, scene->Registry());

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
