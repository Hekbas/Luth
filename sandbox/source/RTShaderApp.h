#pragma once

#include <Luth.h>

#include <memory>
#include <random>
#include <imgui.h>
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
        void OnUpdate() override;
        void OnUIRender() override;
        void OnShutdown() override;

        // Raytracing Shader =============================
        GLuint quadVAO, quadVBO;
        std::shared_ptr<Luth::Shader> shader;
        std::filesystem::path shaderPath;

        // Application state
        int displayMode = 0;
        #define MAX_LIGHTS 4
        #define MAX_SPHERES 6

        struct Camera {
            Vec3 position;
            Vec3 target;
            float fov;
            float orbitRadius;
            float orbitSpeed;
        };

        struct AmbientLight {
            Vec3 skyColor;
            Vec3 groundColor;
            float intensity;
        };

        struct Cloud {
            Vec3 color;
            float density;
            float speed;
            float scale;
        };

        struct Fog {
            bool enabled;
            Vec3 color;
            float density;
            float start;
            float end;
        };

        struct PointLight {
            Vec3 position;
            Vec3 color;
            float intensity;
        };

        struct Material {
            Vec3 albedo;
            Vec3 emissive;
            float roughness;
            float metallic;
            float ior;
            float transparency;
        };

        struct Sphere {
            Vec3 position;
            float r;
            Material mat;
        };

        // Scene config
        Camera camera;
        AmbientLight ambientLight;
        Cloud cloud;
        Fog fog;
        PointLight pointLights[MAX_LIGHTS];
        int numActiveLights = 1;
        Material floorMaterial;
        Sphere spheres[MAX_SPHERES];

        // Rendering
        int ssaaSamples = 4;
        int maxBounces = 5;
        float softShadowFactor = 0.2f;

        // Post-processing
        bool applyTonemap = false;
        bool applyGamma = true;
        float exposure = 1.0f;
        float gammaValue = 1.6f;
        // ===============================================

        void InitScreenQuad();
        void LoadShader();
        void SetVariables();
        void InitUniforms();
        void UpdateUniforms(float time);

        Vec3 CalculatePosition(int max, int i, float r, float y = 1.0);
        float Rand(float min = 0.0f, float max = 1.0f);
        Vec3 RandVec3(float min = 0.0f, float max = 1.0f);

        // ImGui
        bool ExecuteOnButtonPress(const char* label, std::function<void()> callback);
        void ShaderControls();
    };
}
