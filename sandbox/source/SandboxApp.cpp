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
        int currentDisplayMode = 0;
        #define MAX_LIGHTS 4
        #define MAX_SPHERES 32

        struct Material {
            glm::vec3 albedo;
            float roughness;
            float metallic;
        };

        struct Light {
            glm::vec3 position;
            glm::vec3 color;
            float intensity;
        };

        // Scene config
        Material floorMaterial;
        Material sphereMaterials[MAX_SPHERES];
        glm::vec3 spherePositions[MAX_SPHERES];
        Light lights[MAX_LIGHTS];
        int numActiveLights = 1;

        // Rendering
        int ssaaSamples = 4;
        int maxBounces = 2;
        float softShadowFactor = 0.2f;

        // Post-processing
        bool applyTonemapping = true;
        bool applyGamma = true;
        float exposure = 1.0f;
        float gammaValue = 2.2f;
        // ==============================================

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

        void InitUniforms()
        {
            // Display Mode
            shader->SetInt("u_displayMode", 0);

            // Floor Material
            shader->SetVec3("u_floorMaterial.albedo", glm::vec3(0.9f, 0.3f, 0.2f));
            shader->SetFloat("u_floorMaterial.roughness", 0.8f);
            shader->SetFloat("u_floorMaterial.metallic", 0.0f);

            // Sphere Materials
            glm::vec3 spherePositions[3] = {
                {0.0f, 0.0f, 0.0f},
                {2.2f, 0.0f, 0.0f},
                {-2.2f, 0.0f, 0.0f}
            };
            for (int i = 0; i < 3; i++) {
                shader->SetVec3("u_spherePositions[" + std::to_string(i) + "]", spherePositions[i]);
                shader->SetVec3("u_sphereMaterials[" + std::to_string(i) + "].albedo", glm::vec3(0.9f));
                shader->SetFloat("u_sphereMaterials[" + std::to_string(i) + "].roughness", 0.3f);
                shader->SetFloat("u_sphereMaterials[" + std::to_string(i) + "].metallic", 1.0f);
            }

            // Lighting
            shader->SetInt("u_numLights", 1);
            shader->SetVec3("u_lights[0].position", glm::vec3(0.0f, 5.0f, 0.0f));
            shader->SetVec3("u_lights[0].color", glm::vec3(1.0f));
            shader->SetFloat("u_lights[0].intensity", 2.0f);

            // Rendering Parameters
            shader->SetInt("u_SSAA", 4);
            shader->SetInt("u_maxBounces", 2);
            shader->SetFloat("u_softShadowFactor", 0.2f);

            // Post-Processing
            shader->SetInt("u_applyTonemapping", 1);
            shader->SetInt("u_applyGamma", 1);
            shader->SetFloat("u_exposure", 1.0f);
            shader->SetFloat("u_gamma", 2.2f);
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
                    static int displayMode = 0;
                    if (ImGui::Combo("Display Mode", &displayMode, displayModes, IM_ARRAYSIZE(displayModes))) {
                        shader->SetInt("u_displayMode", displayMode);
                    }
                }

                // 2. Scene Configuration
                if (ImGui::CollapsingHeader("Scene Configuration", nodeFlags))
                {
                    static glm::vec3 floorAlbedo(0.9f, 0.3f, 0.2f);
                    static float floorRoughness = 0.8f;
                    static float floorMetallic = 0.0f;

                    if (ImGui::TreeNode("Floor Material")) {
                        if (ImGui::ColorEdit3("Albedo", &floorAlbedo.x)) {
                            shader->SetVec3("u_floorMaterial.albedo", floorAlbedo);
                        }
                        if (ImGui::SliderFloat("Roughness", &floorRoughness, 0.0f, 1.0f)) {
                            shader->SetFloat("u_floorMaterial.roughness", floorRoughness);
                        }
                        if (ImGui::SliderFloat("Metallic", &floorMetallic, 0.0f, 1.0f)) {
                            shader->SetFloat("u_floorMaterial.metallic", floorMetallic);
                        }
                        ImGui::TreePop();
                    }

                    // Sphere controls
                    static glm::vec3 spherePositions[3] = {
                        {0.0f, 0.0f, 0.0f},
                        {2.2f, 0.0f, 0.0f},
                        {-2.2f, 0.0f, 0.0f}
                    };
                    static Material sphereMaterials[3] = {
                        {glm::vec3(0.9f), 0.3f, 1.0f},
                        {glm::vec3(0.9f), 0.3f, 1.0f},
                        {glm::vec3(0.9f), 0.3f, 1.0f}
                    };

                    for (int i = 0; i < 3; i++) {
                        if (ImGui::TreeNode(("Sphere " + std::to_string(i)).c_str())) {
                            // Position
                            if (ImGui::SliderFloat3("Position", &spherePositions[i].x, -5.0f, 5.0f)) {
                                shader->SetVec3("u_spherePositions[" + std::to_string(i) + "]", spherePositions[i]);
                            }

                            // Material
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
                if (ImGui::CollapsingHeader("Lighting", nodeFlags))
                {
                    static int numLights = 1;
                    static Light lights[MAX_LIGHTS] = {
                        {glm::vec3(0.0f, 5.0f, 0.0f), glm::vec3(1.0f), 2.0f}
                    };

                    if (ImGui::SliderInt("Active Lights", &numLights, 1, MAX_LIGHTS)) {
                        shader->SetInt("u_numLights", numLights);
                    }

                    for (int i = 0; i < numLights; i++) {
                        if (ImGui::TreeNode(("Light " + std::to_string(i)).c_str())) {
                            // Position
                            if (ImGui::SliderFloat3("Position", &lights[i].position.x, -10.0f, 10.0f)) {
                                shader->SetVec3("u_lights[" + std::to_string(i) + "].position", lights[i].position);
                            }

                            // Color and intensity
                            if (ImGui::ColorEdit3("Color", &lights[i].color.x)) {
                                shader->SetVec3("u_lights[" + std::to_string(i) + "].color", lights[i].color);
                            }
                            if (ImGui::SliderFloat("Intensity", &lights[i].intensity, 0.0f, 10.0f)) {
                                shader->SetFloat("u_lights[" + std::to_string(i) + "].intensity", lights[i].intensity);
                            }
                            ImGui::TreePop();
                        }
                    }
                }

                // 4. Rendering Parameters
                if (ImGui::CollapsingHeader("Rendering Settings", nodeFlags))
                {
                    static int ssaa = 4;
                    static int bounces = 2;
                    static float shadowSoftness = 0.2f;

                    if (ImGui::SliderInt("SSAA Samples", &ssaa, 1, 16)) {
                        shader->SetInt("u_SSAA", ssaa);
                    }
                    if (ImGui::SliderInt("Max Bounces", &bounces, 1, 8)) {
                        shader->SetInt("u_maxBounces", bounces);
                    }
                    if (ImGui::SliderFloat("Shadow Softness", &shadowSoftness, 0.0f, 1.0f)) {
                        shader->SetFloat("u_softShadowFactor", shadowSoftness);
                    }
                }

                // 5. Post-Processing
                if (ImGui::CollapsingHeader("Post-Processing", nodeFlags))
                {
                    static bool tonemap = true;
                    static bool gamma = true;
                    static float exposure = 1.0f;
                    static float gammaValue = 2.2f;

                    if (ImGui::Checkbox("Tonemapping", &tonemap)) {
                        shader->SetInt("u_applyTonemapping", tonemap ? 1 : 0);
                    }
                    if (ImGui::Checkbox("Gamma Correction", &gamma)) {
                        shader->SetInt("u_applyGamma", gamma ? 1 : 0);
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
