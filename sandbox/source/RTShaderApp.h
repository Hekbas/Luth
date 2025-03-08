#pragma once

#include <Luth.h>

#include <imgui.h>

// TEST
#include <luth/resources/ShaderLibrary.h>
#include <luth/resources/ResourceManager.h>

#include <luth/renderer/Renderer.h>
#include <luth/renderer/Shader.h>
#include <memory>

// TEST GL
#include <GLFW/glfw3.h>
#include <glad/glad.h>
#include <glm/glm.hpp>

namespace Luth
{
    class RTShaderApp : public App
    {
    public:
        RTShaderApp(int argc, char** argv);
        ~RTShaderApp() override = default;

    protected:
        void OnInit() override;
        void OnUpdate(f32 dt) override;
        void OnUIRender() override;
        void OnShutdown() override;

        // Raytracing Shader =============================
        GLuint quadVAO, quadVBO;
        std::shared_ptr<Luth::Shader> shader;
        std::filesystem::path shaderPath;

        // Application state
        int displayMode = 0;
        #define MAX_LIGHTS 4
        #define MAX_SPHERES 32

        struct Camera {
            Vec3 origin;
            Vec3 direction;
            Vec3 lookAt;
            float fov;
            bool useLookAt;
        };

        struct AmbientLight {
            Vec3 skyColor;
            Vec3 groundColor;
            float intensity;
        };

        struct PointLight {
            Vec3 position;
            Vec3 color;
            float intensity;
        };
        
        struct Fog {
            bool enabled;
            Vec3 color;
            float density;
            float start;
            float end;
        };

        struct Material {
            Vec3 albedo;
            Vec3 emissive;
            float roughness;
            float metallic;
            float ior;
            float transparency;
        };

        // Scene config
        Camera camera;
        AmbientLight ambientLight;
        PointLight pointLights[MAX_LIGHTS];
        int numActiveLights = 1;
        Fog fog;
        Material floorMaterial;
        glm::vec3 spherePositions[MAX_SPHERES];
        Material sphereMaterials[MAX_SPHERES];

        // Rendering
        int ssaaSamples = 4;
        int maxBounces = 2;
        float softShadowFactor = 0.2f;

        // Post-processing
        bool applyTonemap = true;
        bool applyGamma = true;
        float exposure = 1.0f;
        float gammaValue = 2.2f;
        // ===============================================

        void InitScreenQuad();
        void LoadShader();
        void SetVariables();
        void InitUniforms();
        void UpdateUniforms(float time);

        // ImGui
        bool ExecuteOnButtonPress(const char* label, std::function<void()> callback);
        void ShaderControls();
    };
}
