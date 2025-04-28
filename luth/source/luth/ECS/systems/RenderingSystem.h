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

        void SetViewProjection(const Mat4& vp);

    private:
        std::pair<std::vector<RenderCommand>, std::vector<RenderCommand>>
            CollectCommands(entt::registry& registry);

        void UpdateLightsUBO(entt::registry& registry);
        void UpdateTransformUBO(const Mat4& view, const Mat4& projection, const Mat4& model);

        std::unordered_map<std::string, std::shared_ptr<RenderTechnique>> m_MainTechniques;
        std::shared_ptr<RenderTechnique> m_ActiveTechnique;

        std::unordered_map<std::string, std::shared_ptr<RenderTechnique>> m_OverlayTechniques;
        std::unordered_set<std::string> m_EnabledOverlays;

        Vec3 m_CameraPos;
        Mat4 m_ViewProjection;
        u32 m_TransformUBO;
        u32 m_LightsUBO;
    };

    #define MAX_DIR_LIGHTS 4
    #define MAX_POINT_LIGHTS 16

    struct DirLightUBO {
        alignas(16) Vec3 color;
        float intensity;
        alignas(16) Vec3 direction;
        float padding;
    };

    struct PointLightUBO {
        alignas(16) Vec3 color;
        float intensity;
        alignas(16) Vec3 position;
        float range;
    };

    struct LightsUBO {
        int dirLightCount = 0;
        DirLightUBO dirLights[MAX_DIR_LIGHTS];

        int pointLightCount = 0;
        PointLightUBO pointLights[MAX_POINT_LIGHTS];
    };

    struct TransformUBO {
        glm::mat4 view;
        glm::mat4 projection;
        glm::mat4 model;

        TransformUBO() : view(1.0f), projection(1.0f), model(1.0f) {}
    };
}
