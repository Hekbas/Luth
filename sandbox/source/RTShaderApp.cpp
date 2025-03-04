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
        RTShaderApp(int argc, char** argv) : App(argc, argv) {}
        ~RTShaderApp() override = default;

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
            Vec3 emissive;
            float roughness;
            float metallic;
            float ior;
            float transparency;
        };

        struct AmbientLight {
            glm::vec3 skyColor;
            glm::vec3 groundColor;
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
            camera.origin = glm::vec3(0.0, 2.2, 5.3);
            camera.direction = glm::vec3(0.0, 7.3, 34.0);
            camera.lookAt = glm::vec3(0.0, 0.0, 0.0);
            camera.fov = 110.0f;
            camera.useLookAt = false;

            // Floor
            floorMaterial.albedo = glm::vec3(1.0f, 1.0f, 1.0f);
            floorMaterial.roughness = 0.8f;
            floorMaterial.metallic = 0.1f;
            floorMaterial.ior = 1.0f;
            floorMaterial.transparency = 0.0f;

            // Spheres
            spherePositions[0] = { 2.2f, 0.6f, 0.0f };
            spherePositions[1] = { 0.0f, 0.6f, 0.0f };
            spherePositions[2] = { -2.2f, 0.6f, 0.0f };

            // Sphere Materials
            sphereMaterials[0].albedo = glm::vec3(1.0f, 0.0f, 0.0f);
            sphereMaterials[0].emissive = glm::vec3(0.0f, 0.0f, 0.0f);
            sphereMaterials[0].roughness = 0.05;
            sphereMaterials[0].metallic = 0.05;
            sphereMaterials[0].ior = 1.5;
            sphereMaterials[0].transparency = 0.0;

            sphereMaterials[1].albedo = glm::vec3(0.0f, 1.0f, 0.0f);
            sphereMaterials[1].emissive = glm::vec3(0.0f, 0.0f, 0.0f);
            sphereMaterials[1].roughness = 0.05;
            sphereMaterials[1].metallic = 1.0;
            sphereMaterials[1].ior = 1.5;
            sphereMaterials[1].transparency = 0.95;

            sphereMaterials[2].albedo = glm::vec3(0.0f, 0.0f, 1.0f);
            sphereMaterials[2].emissive = glm::vec3(0.0f, 0.0f, 0.0f);
            sphereMaterials[2].roughness = 1.0;
            sphereMaterials[2].metallic = 0.05;
            sphereMaterials[2].ior = 1.5;
            sphereMaterials[2].transparency = 0.0;

            // Lighting
            ambientLight.skyColor = glm::vec3(0.0, 0.0, 0.36);
            ambientLight.groundColor = glm::vec3(1.0, 0.42, 0.0);
            ambientLight.intensity = 1.0f;

            pointLights[0].position = glm::vec3(3.22f, 3.9f, 2.37f);
            pointLights[0].color = glm::vec3(1.0f);
            pointLights[0].intensity = 10.0f;
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
            shader->SetFloat("u_floorMaterial.ior", floorMaterial.ior);
            shader->SetFloat("u_floorMaterial.transparency", floorMaterial.transparency);

            // Sphere Materials
            for (int i = 0; i < 3; i++) {
                shader->SetVec3("u_spherePositions[" + std::to_string(i) + "]", spherePositions[i]);
                shader->SetVec3("u_sphereMaterials[" + std::to_string(i) + "].albedo", sphereMaterials[i].albedo);
                shader->SetVec3("u_sphereMaterials[" + std::to_string(i) + "].emissive", sphereMaterials[i].emissive);
                shader->SetFloat("u_sphereMaterials[" + std::to_string(i) + "].roughness", sphereMaterials[i].roughness);
                shader->SetFloat("u_sphereMaterials[" + std::to_string(i) + "].metallic", sphereMaterials[i].metallic);
                shader->SetFloat("u_sphereMaterials[" + std::to_string(i) + "].ior", sphereMaterials[i].ior);
                shader->SetFloat("u_sphereMaterials[" + std::to_string(i) + "].transparency", sphereMaterials[i].transparency);
            }

            // Lighting
            shader->SetVec3("u_ambientLight.skyColor", ambientLight.skyColor);
            shader->SetVec3("u_ambientLight.groundColor", ambientLight.groundColor);
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
                        "World Position",
                        "Normals",
                        "Fressnel",
                        "Radiance",
                        "Diffuse",
                        "Specular"
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

                    // Floor
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
                        if (ImGui::SliderFloat("IOR", &floorMaterial.ior, 0.0f, 5.0f)) {
                            shader->SetFloat("u_floorMaterial.ior", floorMaterial.ior);
                        }
                        if (ImGui::SliderFloat("Transparency", &floorMaterial.transparency, 0.0f, 1.0f)) {
                            shader->SetFloat("u_floorMaterial.transparency", floorMaterial.transparency);
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
                            if (ImGui::ColorEdit3("Emissive", &sphereMaterials[i].emissive.x)) {
                                shader->SetVec3("u_sphereMaterials[" + std::to_string(i) + "].emissive",
                                    sphereMaterials[i].emissive);
                            }
                            if (ImGui::SliderFloat("Roughness", &sphereMaterials[i].roughness, 0.0f, 1.0f)) {
                                shader->SetFloat("u_sphereMaterials[" + std::to_string(i) + "].roughness",
                                    sphereMaterials[i].roughness);
                            }
                            if (ImGui::SliderFloat("Metallic", &sphereMaterials[i].metallic, 0.0f, 1.0f)) {
                                shader->SetFloat("u_sphereMaterials[" + std::to_string(i) + "].metallic",
                                    sphereMaterials[i].metallic);
                            }
                            if (ImGui::SliderFloat("IOR", &sphereMaterials[i].ior, 0.0f, 5.0f)) {
                                shader->SetFloat("u_sphereMaterials[" + std::to_string(i) + "].ior",
                                    sphereMaterials[i].ior);
                            }
                            if (ImGui::SliderFloat("Transparency", &sphereMaterials[i].transparency, 0.0f, 1.0f)) {
                                shader->SetFloat("u_sphereMaterials[" + std::to_string(i) + "].transparency",
                                    sphereMaterials[i].transparency);
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
                    if (ImGui::ColorEdit3("Sky Color", &ambientLight.skyColor.x)) {
                        shader->SetVec3("u_ambientLight.skyColor", ambientLight.skyColor);
                    }
                    if (ImGui::ColorEdit3("Ground Color", &ambientLight.groundColor.x)) {
                        shader->SetVec3("u_ambientLight.groundColor", ambientLight.groundColor);
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
}
