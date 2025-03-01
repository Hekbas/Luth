#include "Luth.h"

#include <imgui.h>

// TEST
#include "luth/resources/ShaderLibrary.h"
#include "luth/resources/ResourceManager.h"

#include "luth/renderer/Renderer.h"
#include "luth/renderer/vulkan/VKRendererAPI.h"
#include "luth/renderer/Shader.h"
#include <memory>

// TEST GL / VULKAN
#define GLFW_INCLUDE_VULKAN
#include <GLFW/glfw3.h>
#include <glad/glad.h>
#include <glm/glm.hpp>

namespace Luth
{
    class SandboxApp : public App
    {
    public:
        SandboxApp() {}
        ~SandboxApp() override = default;

    protected:
        void OnInit() override
        {
            InitScreenQuad();
            SetVariables();
            LoadShader();
        }

        void OnUpdate(f32 dt) override
        {
            static float time = 0;
            time += dt;

            UpdateUniforms(time);
        }

        void OnUIRender() override
        {
            // ImGui Demo
            static bool showDemo = true;
            ShaderControls();
            if (showDemo) ImGui::ShowDemoWindow(&showDemo);
        }

        void OnShutdown() override
        {
            //vkDestroyInstance(instance, nullptr);
        }

        // OPENGL
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

        struct Material {
            Vec3 albedo;
            float roughness;
            float metallic;
        };

        struct AmbientLight {
            glm::vec3 color;
            float intensity;
        };

        struct PointLight {
            glm::vec3 position;
            glm::vec3 color;
            float intensity;
        };

        // Scene config
        Camera camera;
        Material floorMaterial;
        Material sphereMaterials[MAX_SPHERES];
        glm::vec3 spherePositions[MAX_SPHERES];
        AmbientLight ambientLight;
        PointLight pointLights[MAX_LIGHTS];
        int numActiveLights = 1;

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

        void InitScreenQuad()
        {
            float vertices[] =
            {
                // Positions   // TexCoords
                -1.0f,  1.0f,  0.0f, 1.0f,
                -1.0f, -1.0f,  0.0f, 0.0f,
                 1.0f, -1.0f,  1.0f, 0.0f,

                -1.0f,  1.0f,  0.0f, 1.0f,
                 1.0f, -1.0f,  1.0f, 0.0f,
                 1.0f,  1.0f,  1.0f, 1.0f
            };

            glGenVertexArrays(1, &quadVAO);
            glGenBuffers(1, &quadVBO);

            glBindVertexArray(quadVAO);
            glBindBuffer(GL_ARRAY_BUFFER, quadVBO);
            glBufferData(GL_ARRAY_BUFFER, sizeof(vertices), vertices, GL_STATIC_DRAW);

            // Position attribute
            glEnableVertexAttribArray(0);
            glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), (void*)0);

            // Texture coordinate attribute
            glEnableVertexAttribArray(1);
            glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), (void*)(2 * sizeof(float)));

            glBindVertexArray(0);
        }

        void LoadShader()
        {
            std::filesystem::path shaderPath = ResourceManager::GetPath(ResourceManager::ResourceType::Shader, "raytracing.glsl");
            shader = Shader::Create(shaderPath.generic_string());
            shader->Bind();
            InitUniforms();
        }

        void SetVariables()
        {
            // Camera
            camera.origin = glm::vec3(0.0, 0.5, 8.0);
            camera.direction = glm::vec3(0.0, 0.0, 10.0);
            camera.lookAt = glm::vec3(0.0, 0.0, 0.0);
            camera.fov = 60.0f;
            camera.useLookAt = false;

            // Floor
            floorMaterial.albedo = glm::vec3(0.9f, 0.3f, 0.2f);
            floorMaterial.roughness = 0.8f;
            floorMaterial.metallic = 0.0f;

            // Spheres
            spherePositions[0] = { 0.0f, 0.6f, 0.0f };
            spherePositions[1] = { 2.2f, 0.6f, 0.0f };
            spherePositions[2] = { -2.2f, 0.6f, 0.0f };

            // Sphere Materials
            for (int i = 0; i < 3; i++) {
                sphereMaterials[i].albedo = glm::vec3(0.9f);
                sphereMaterials[i].metallic = 0.3f;
                sphereMaterials[i].roughness = 1.0f;
            }

            // Lighting
            ambientLight.color = glm::vec3(0.5, 0.7, 1.0);
            ambientLight.intensity = 1.0f;

            pointLights[0].position = glm::vec3(0.0f, 5.0f, 0.0f);
            pointLights[0].color = glm::vec3(1.0f);
            pointLights[0].intensity = 2.0f;
        }

        void InitUniforms()
        {
            // Display Mode
            shader->SetInt("u_displayMode", displayMode);

            // Camera
            shader->SetVec3("u_camera.origin", camera.origin);
            shader->SetVec3("u_camera.direction", camera.direction);
            shader->SetVec3("u_camera.lookAt", camera.lookAt);
            shader->SetFloat("u_camera.fov", camera.fov);

            // Floor Material
            shader->SetVec3("u_floorMaterial.albedo", floorMaterial.albedo);
            shader->SetFloat("u_floorMaterial.roughness", floorMaterial.roughness);
            shader->SetFloat("u_floorMaterial.metallic", floorMaterial.metallic);

            // Sphere Materials
            for (int i = 0; i < 3; i++) {
                shader->SetVec3("u_spherePositions[" + std::to_string(i) + "]", spherePositions[i]);
                shader->SetVec3("u_sphereMaterials[" + std::to_string(i) + "].albedo", sphereMaterials[i].albedo);
                shader->SetFloat("u_sphereMaterials[" + std::to_string(i) + "].roughness", sphereMaterials[i].metallic);
                shader->SetFloat("u_sphereMaterials[" + std::to_string(i) + "].metallic", sphereMaterials[i].roughness);
            }

            // Lighting
            shader->SetVec3("u_ambientLight.color", ambientLight.color);
            shader->SetFloat("u_ambientLight.intensity", ambientLight.intensity);

            shader->SetInt("u_numPointLights", numActiveLights);
            shader->SetVec3("u_pointLights[0].position", pointLights[0].position);
            shader->SetVec3("u_pointLights[0].color", pointLights[0].color);
            shader->SetFloat("u_pointLights[0].intensity", pointLights[0].intensity);

            // Rendering Parameters
            shader->SetInt("u_SSAA", ssaaSamples);
            shader->SetInt("u_maxBounces", maxBounces);
            shader->SetFloat("u_softShadowFactor", softShadowFactor);

            // Post-Processing
            shader->SetInt("u_applyTonemapping", applyTonemap);
            shader->SetInt("u_applyGamma", applyGamma);
            shader->SetFloat("u_exposure", exposure);
            shader->SetFloat("u_gamma", gammaValue);
        }

        void UpdateUniforms(float time)
        {
            shader->SetFloat("u_time", time);
            shader->SetVec2("u_resolution",  glm::vec2(1280.0, 720.0));
            glBindVertexArray(quadVAO);
            glDrawArrays(GL_TRIANGLES, 0, 6);
        }

        // ImGui
        bool ExecuteOnButtonPress(const char* label, std::function<void()> callback)
        {
            if (ImGui::Button(label))
            {
                if (callback) callback();
                return true;
            }
            return false;
        }

        void ShaderControls()
        {
            ImGuiTreeNodeFlags nodeFlags = ImGuiTreeNodeFlags_DefaultOpen;

            if (ImGui::Begin("Raytracing Controls"))
            {
                // 0. Reload Shader
                ExecuteOnButtonPress("Reload Shader", [this]() { LoadShader(); });

                // 1. Display Modes
                if (ImGui::CollapsingHeader("Visualization Modes", nodeFlags))
                {
                    const char* displayModes[] = {
                        "Final Render",
                        "Ray Directions",
                        "Albedo",
                        "Specular",
                        "Radiance",
                        "Normals",
                        "World Position"
                    };
                    if (ImGui::Combo("Display Mode", &displayMode, displayModes, IM_ARRAYSIZE(displayModes))) {
                        shader->SetInt("u_displayMode", displayMode);
                    }
                }

                // 2. Scene 
                if (ImGui::CollapsingHeader("Scene", nodeFlags))
                {
                    // Camera
                    if (ImGui::TreeNode("Camera")) {
                        if (ImGui::SliderFloat3("Position", &camera.origin.x, -10.0f, 10.0f)) {
                            shader->SetVec3("u_camera.origin", camera.origin);
                        }
                        if (ImGui::SliderFloat3("Direction", &camera.direction.x, -50.0f, 50.0f)) {
                            shader->SetVec3("u_camera.direction", camera.direction);
                        }
                        if (ImGui::SliderFloat3("Look At", &camera.lookAt.x, -50.0f, 50.0f)) {
                            shader->SetVec3("u_camera.lookAt", camera.lookAt);
                        }
                        if (ImGui::SliderFloat("FOV", &camera.fov, 10.0f, 140.0f)) {
                            shader->SetFloat("u_camera.fov", camera.fov);
                        }
                        if (ImGui::Checkbox("Use LookAt", &camera.useLookAt)) {
                            shader->SetInt("u_camera.useLookAt", camera.useLookAt ? 1 : 0);
                        }
                        ImGui::TreePop();
                    }

                    //Floor
                    if (ImGui::TreeNode("Floor Material")) {
                        if (ImGui::ColorEdit3("Albedo", &floorMaterial.albedo.x)) {
                            shader->SetVec3("u_floorMaterial.albedo", floorMaterial.albedo);
                        }
                        if (ImGui::SliderFloat("Roughness", &floorMaterial.roughness, 0.0f, 1.0f)) {
                            shader->SetFloat("u_floorMaterial.roughness", floorMaterial.roughness);
                        }
                        if (ImGui::SliderFloat("Metallic", &floorMaterial.metallic, 0.0f, 1.0f)) {
                            shader->SetFloat("u_floorMaterial.metallic", floorMaterial.metallic);
                        }
                        ImGui::TreePop();
                    }

                    // Sphere controls
                    for (int i = 0; i < 3; i++) {
                        if (ImGui::TreeNode(("Sphere " + std::to_string(i)).c_str())) {
                            if (ImGui::SliderFloat3("Position", &spherePositions[i].x, -5.0f, 5.0f)) {
                                shader->SetVec3("u_spherePositions[" + std::to_string(i) + "]", spherePositions[i]);
                            }
                            if (ImGui::ColorEdit3("Albedo", &sphereMaterials[i].albedo.x)) {
                                shader->SetVec3("u_sphereMaterials[" + std::to_string(i) + "].albedo",
                                    sphereMaterials[i].albedo);
                            }
                            if (ImGui::SliderFloat("Roughness", &sphereMaterials[i].roughness, 0.0f, 1.0f)) {
                                shader->SetFloat("u_sphereMaterials[" + std::to_string(i) + "].roughness",
                                    sphereMaterials[i].roughness);
                            }
                            if (ImGui::SliderFloat("Metallic", &sphereMaterials[i].metallic, 0.0f, 1.0f)) {
                                shader->SetFloat("u_sphereMaterials[" + std::to_string(i) + "].metallic",
                                    sphereMaterials[i].metallic);
                            }
                            ImGui::TreePop();
                        }
                    }
                }

                // 3. Lighting Controls
                
                // Ambient Light
                if (ImGui::CollapsingHeader("Ambient Light", nodeFlags))
                {
                    // Color and intensity
                    if (ImGui::ColorEdit3("Color", &ambientLight.color.x)) {
                        shader->SetVec3("u_ambientLight.color", ambientLight.color);
                    }
                    if (ImGui::SliderFloat("Intensity", &ambientLight.intensity, 0.0f, 10.0f)) {
                        shader->SetFloat("u_ambientLight.intensity", ambientLight.intensity);
                    }
                }

                // Point Lights
                if (ImGui::CollapsingHeader("Point Lights", nodeFlags))
                {
                    if (ImGui::SliderInt("Active Lights", &numActiveLights, 1, MAX_LIGHTS)) {
                        shader->SetInt("u_numPointLights", numActiveLights);
                    }

                    for (int i = 0; i < numActiveLights; i++) {
                        if (ImGui::TreeNode(("Light " + std::to_string(i)).c_str())) {
                            // Position
                            if (ImGui::SliderFloat3("Position", &pointLights[i].position.x, -10.0f, 10.0f)) {
                                shader->SetVec3("u_pointLights[" + std::to_string(i) + "].position", pointLights[i].position);
                            }

                            // Color and intensity
                            if (ImGui::ColorEdit3("Color", &pointLights[i].color.x)) {
                                shader->SetVec3("u_pointLights[" + std::to_string(i) + "].color", pointLights[i].color);
                            }
                            if (ImGui::SliderFloat("Intensity", &pointLights[i].intensity, 0.0f, 10.0f)) {
                                shader->SetFloat("u_pointLights[" + std::to_string(i) + "].intensity", pointLights[i].intensity);
                            }
                            ImGui::TreePop();
                        }
                    }
                }

                // 4. Rendering Parameters
                if (ImGui::CollapsingHeader("Rendering Settings", nodeFlags))
                {

                    if (ImGui::SliderInt("SSAA Samples", &ssaaSamples, 1, 16)) {
                        shader->SetInt("u_SSAA", ssaaSamples);
                    }
                    if (ImGui::SliderInt("Max Bounces", &maxBounces, 0, 8)) {
                        shader->SetInt("u_maxBounces", maxBounces);
                    }
                    if (ImGui::SliderFloat("Shadow Softness", &softShadowFactor, 0.0f, 1.0f)) {
                        shader->SetFloat("u_softShadowFactor", softShadowFactor);
                    }
                }

                // 5. Post-Processing
                if (ImGui::CollapsingHeader("Post-Processing", nodeFlags))
                {
                    if (ImGui::Checkbox("Tonemapping", &applyTonemap)) {
                        shader->SetInt("u_applyTonemapping", applyTonemap ? 1 : 0);
                    }
                    if (ImGui::Checkbox("Gamma Correction", &applyGamma)) {
                        shader->SetInt("u_applyGamma", applyGamma ? 1 : 0);
                    }
                    if (ImGui::SliderFloat("Exposure", &exposure, 0.1f, 5.0f)) {
                        shader->SetFloat("u_exposure", exposure);
                    }
                    if (ImGui::SliderFloat("Gamma Value", &gammaValue, 1.0f, 3.0f)) {
                        shader->SetFloat("u_gamma", gammaValue);
                    }
                }
            }

            ImGui::End();
        }
    };

    App* CreateApp()
    {
        return new SandboxApp();
    }
}

int main()
{
    Luth::Log::Init();
    Luth::App* app = Luth::CreateApp();
    app->Run();
    delete app;
    return 0;
}
