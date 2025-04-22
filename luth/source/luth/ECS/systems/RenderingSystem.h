#pragma once

#include "luth/ECS/System.h"
#include "luth/renderer/RenderTechnique.h"

#include <entt/entt.hpp>

namespace Luth
{
    class RenderingSystem : public System
    {
    public:
        RenderingSystem();

        void Update(entt::registry& registry) override;
        void Resize(u32 width, u32 height);

        void RegisterMainTechnique(const std::string& name, std::shared_ptr<RenderTechnique> technique);
        void SetTechnique(const std::string& name);
        std::shared_ptr<RenderTechnique> GetActiveTechnique() { return m_ActiveTechnique; }
        const auto& GetAvailableTechniques() const { return m_MainTechniques; }

        void RegisterOverlayTechnique(const std::string& name, std::shared_ptr<RenderTechnique> tech) {
            m_OverlayTechniques[name] = tech;
        }
        void EnableOverlay(const std::string& name) { m_EnabledOverlays.insert(name); }
        void DisableOverlay(const std::string& name) { m_EnabledOverlays.erase(name); }
        bool IsOverlayEnabled(const std::string& name) const { return m_EnabledOverlays.count(name); }

        void SetViewProjection(const glm::mat4& vp);

    private:
        std::pair<std::vector<RenderCommand>, std::vector<RenderCommand>>
            CollectCommands(entt::registry& registry);

        std::unordered_map<std::string, std::shared_ptr<RenderTechnique>> m_MainTechniques;
        std::shared_ptr<RenderTechnique> m_ActiveTechnique;

        std::unordered_map<std::string, std::shared_ptr<RenderTechnique>> m_OverlayTechniques;
        std::unordered_set<std::string> m_EnabledOverlays;

        Vec3 m_CameraPos;
        Mat4 m_ViewProjection;
    };
}
