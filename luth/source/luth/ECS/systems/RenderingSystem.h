#pragma once

#include "luth/ECS/System.h"
#include "luth/renderer/pipeline/RenderPipeline.h"
#include "luth/renderer/pipeline/RenderPass.h"

#include <entt/entt.hpp>
#include <unordered_map>

namespace Luth
{
    class RenderingSystem : public System
    {
    public:
        RenderingSystem(u32 viewportWidth = 1280, u32 viewportHeight = 720);

        void Update(entt::registry& registry) override;
        void Resize(u32 width, u32 height);

        // Technique = pipeline of passes
        void RegisterTechnique(const std::string& name, RenderPipeline&& pipeline);
        void SetActiveTechnique(const std::string& name);

        std::vector<std::string> GetTechniqueNames() const;
        const std::string& GetActiveTechniqueName() const;
        RenderPipeline* GetActivePipeline() const { return m_ActivePipeline; }

    private:
        std::pair<std::vector<RenderCommand>, std::vector<RenderCommand>>
            CollectCommands(entt::registry& registry);
        void UpdateTransformUBO(const Mat4& view, const Mat4& proj, const Mat4& model);
        void UpdateLightsUBO(entt::registry& registry);

        // map of ready-to-go pipelines
        std::unordered_map<std::string, RenderPipeline> m_Pipelines;
        RenderPipeline* m_ActivePipeline = nullptr;
        std::string m_ActiveName;

        // UBOs, camera, etc.
        u32   m_TransformUBO, m_LightsUBO;
        Vec3  m_CameraPos;
        Mat4  m_ViewProj;
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
		TransformUBO(Mat4 view, Mat4 projection, Mat4 model)
			: view(view), projection(projection), model(model) {}
    };
}
