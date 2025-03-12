#include <Luth.h>
#include "RTShaderApp.h"

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
    RTShaderApp::RTShaderApp(int argc, char** argv) : App(argc, argv) {}

    void RTShaderApp::OnInit()
    {
        InitScreenQuad();
        SetVariables();
        LoadShader();
    }

    void RTShaderApp::OnUpdate(f32 dt)
    {
        static float time = 0;
        time += dt;

        UpdateUniforms(time);
    }

    void RTShaderApp::OnUIRender()
    {
        // ImGui Demo
        static bool showDemo = false;
        ShaderControls();
        if (showDemo) ImGui::ShowDemoWindow(&showDemo);

        ImGuiIO& io = ImGui::GetIO();
        ImGui::Begin("Engine Dashboard");
        ImGui::Text("App avg %.3f ms/frame (%.1f FPS)", 1000.0f / io.Framerate, io.Framerate);
        ImGui::End();
    }

    void RTShaderApp::OnShutdown()
    {
        //vkDestroyInstance(instance, nullptr);
    }

    void RTShaderApp::InitScreenQuad()
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

    void RTShaderApp::LoadShader()
    {
        std::filesystem::path shaderPath = ResourceManager::GetPath(ResourceManager::ResourceType::Shader, "raytracing.glsl");
        shader = Shader::Create(shaderPath.generic_string());
        shader->Bind();
        InitUniforms();
    }

    void RTShaderApp::SetVariables()
    {
        // Camera
        camera.origin = glm::vec3(0.0, 5.6, 6.0);
        camera.direction = glm::vec3(0.0, 15.5, 34.0);
        camera.lookAt = glm::vec3(0.0, 0.0, 0.0);
        camera.fov = 110.0f;
        camera.useLookAt = false;

        // Floor
        floorMaterial.albedo = glm::vec3(1.0f, 1.0f, 1.0f);
        floorMaterial.roughness = 0.8f;
        floorMaterial.metallic = 0.1f;
        floorMaterial.ior = 1.0f;
        floorMaterial.transparency = 0.0f;

        // Sphere Positions
        spheres[0].position = CalculatePosition(3, 0, 2.5, 2);
        spheres[1].position = CalculatePosition(3, 1, 2.5, 2);
        spheres[2].position = CalculatePosition(3, 2, 2.5, 2);
        spheres[3].position = CalculatePosition(3, 3, 2.5, 5);
        spheres[4].position = CalculatePosition(3, 4, 2.5, 5);
        spheres[5].position = CalculatePosition(3, 5, 2.5, 5);

        // Sphere Radius
        spheres[0].r = 1.0f;
        spheres[1].r = 0.8f;
        spheres[2].r = 0.6f;
        spheres[3].r = 1.2f;
        spheres[4].r = 0.6f;
        spheres[5].r = 0.8f;

        // Sphere Materials
        spheres[0].mat.albedo = glm::vec3(1.0f, 0.0f, 0.0f);
        spheres[0].mat.emissive = glm::vec3(0.1f, 0.8f, 0.35f);
        spheres[0].mat.roughness = 0.05;
        spheres[0].mat.metallic = 0.05;
        spheres[0].mat.ior = 1.5;
        spheres[0].mat.transparency = 0.0;

        spheres[1].mat.albedo = glm::vec3(0.0f, 1.0f, 0.0f);
        spheres[1].mat.emissive = glm::vec3(0.0f, 0.0f, 0.0f);
        spheres[1].mat.roughness = 0.05;
        spheres[1].mat.metallic = 1.0;
        spheres[1].mat.ior = 1.5;
        spheres[1].mat.transparency = 0.95;

        spheres[2].mat.albedo = glm::vec3(0.0f, 0.0f, 0.0f);
        spheres[2].mat.emissive = glm::vec3(0.0f, 0.0f, 0.0f);
        spheres[2].mat.roughness = 0.9;
        spheres[2].mat.metallic = 0.5;
        spheres[2].mat.ior = 1.5;
        spheres[2].mat.transparency = 0.0;

        spheres[3].mat.albedo = glm::vec3(0.87f, 0.06f, 0.06f);
        spheres[3].mat.emissive = glm::vec3(0.0f, 0.0f, 0.0f);
        spheres[3].mat.roughness = 0.05;
        spheres[3].mat.metallic = 0.05;
        spheres[3].mat.ior = 1.5;
        spheres[3].mat.transparency = 0.0;

        spheres[4].mat.albedo = glm::vec3(1.0f, 1.0f, 1.0f);
        spheres[4].mat.emissive = glm::vec3(0.0f, 0.0f, 0.0f);
        spheres[4].mat.roughness = 0.1;
        spheres[4].mat.metallic = 1.0;
        spheres[4].mat.ior = 1.8;
        spheres[4].mat.transparency = 1.0;

        spheres[5].mat.albedo = glm::vec3(0.0f, 0.0f, 1.0f);
        spheres[5].mat.emissive = glm::vec3(0.0f, 0.0f, 0.0f);
        spheres[5].mat.roughness = 0.035;
        spheres[5].mat.metallic = 0.9;
        spheres[5].mat.ior = 1.5;
        spheres[5].mat.transparency = 0.0;

        // Environment Light
        ambientLight.skyColor = glm::vec3(0.0, 0.0, 0.36);
        ambientLight.groundColor = glm::vec3(1.0, 0.42, 0.0);
        ambientLight.intensity = 1.0f;

        // Environment Fog
        fog.enabled = false;
        fog.color = glm::vec3(1.0f, 1.0f, 1.0f);
        fog.density = 1.0;
        fog.start = 5.0;
        fog.end = 50.0;

        // Point Lights
        for (size_t i = 0; i < MAX_LIGHTS; i++)
        {
            pointLights[i].position = glm::vec3(3.22f, 3.9f, 2.37f);
            pointLights[i].color = glm::vec3(1.0f);
            pointLights[i].intensity = 10.0f;
        }
    }

    void RTShaderApp::InitUniforms()
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

        // Sphere Data
        for (int i = 0; i < MAX_SPHERES; i++) {
            shader->SetVec3("u_spheres[" + std::to_string(i) + "].position", spheres[i].position);
            shader->SetFloat("u_spheres[" + std::to_string(i) + "].r", spheres[i].r);
            shader->SetVec3("u_spheres[" + std::to_string(i) + "].mat.albedo", spheres[i].mat.albedo);
            shader->SetVec3("u_spheres[" + std::to_string(i) + "].mat.emissive", spheres[i].mat.emissive);
            shader->SetFloat("u_spheres[" + std::to_string(i) + "].mat.roughness", spheres[i].mat.roughness);
            shader->SetFloat("u_spheres[" + std::to_string(i) + "].mat.metallic", spheres[i].mat.metallic);
            shader->SetFloat("u_spheres[" + std::to_string(i) + "].mat.ior", spheres[i].mat.ior);
            shader->SetFloat("u_spheres[" + std::to_string(i) + "].mat.transparency", spheres[i].mat.transparency);
        }

        // Environment Light
        shader->SetVec3("u_ambientLight.skyColor", ambientLight.skyColor);
        shader->SetVec3("u_ambientLight.groundColor", ambientLight.groundColor);
        shader->SetFloat("u_ambientLight.intensity", ambientLight.intensity);

        // Environment Clouds
        // TODO add cloud controls

        // Environment Fog
        shader->SetBool("u_fog.enabled", fog.enabled);
        shader->SetVec3("u_fog.color", fog.color);
        shader->SetFloat("u_fog.density", fog.density);
        shader->SetFloat("u_fog.start", fog.start);
        shader->SetFloat("u_fog.end", fog.end);

        // Point Lights
        shader->SetInt("u_numPointLights", numActiveLights);
        for (int i = 0; i < MAX_LIGHTS; i++)
        {
            shader->SetVec3("u_pointLights[" + std::to_string(i) + "].position", pointLights[i].position);
            shader->SetVec3("u_pointLights[" + std::to_string(i) + "].color", pointLights[i].color);
            shader->SetFloat("u_pointLights[" + std::to_string(i) + "].intensity", pointLights[i].intensity);
        }

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

    void RTShaderApp::UpdateUniforms(float time)
    {
        shader->SetFloat("u_time", time);
        shader->SetVec2("u_resolution", glm::vec2(1280.0, 720.0));
        glBindVertexArray(quadVAO);
        glDrawArrays(GL_TRIANGLES, 0, 6);
    }
    
    // Helpers
    Vec3 RTShaderApp::CalculatePosition(int max, int i, float r, float y) {
        const float pi = std::acos(-1.0f);
        float angle = (2.0f * pi * static_cast<float>(i)) / static_cast<float>(max);
        float x = r * std::cosf(angle);
        float z = r * std::sinf(angle);
        return { x, y, z };
    }

    float RTShaderApp::Rand(float min, float max) {
        // Random number setup (Mersenne Twister engine)
        static std::mt19937 engine(std::random_device{}());
        std::uniform_real_distribution<float> distribution(min, max);

        return distribution(engine);
    }

    Vec3 RTShaderApp::RandVec3(float min, float max) {
        // Random number setup (Mersenne Twister engine)
        static std::mt19937 engine(std::random_device{}());
        std::uniform_real_distribution<float> distribution(min, max);

        return glm::vec3(
            distribution(engine),
            distribution(engine),
            distribution(engine)
        );
    }

    // ImGui
    bool RTShaderApp::ExecuteOnButtonPress(const char* label, std::function<void()> callback)
    {
        if (ImGui::Button(label))
        {
            if (callback) callback();
            return true;
        }
        return false;
    }

    void RTShaderApp::ShaderControls()
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
                    "Specular",
                    "Emissive",
                    "Depth"
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
                for (int i = 0; i < MAX_SPHERES; i++) {
                    if (ImGui::TreeNode(("Sphere " + std::to_string(i)).c_str())) {
                        if (ImGui::SliderFloat3("Position", &spheres[i].position.x, -5.0f, 5.0f)) {
                            shader->SetVec3("u_spheres[" + std::to_string(i) + "].position", spheres[i].position);
                        }
                        if (ImGui::SliderFloat("Radius", &spheres[i].r, 0.1f, 2.0f)) {
                            shader->SetFloat("u_spheres[" + std::to_string(i) + "].r",
                                spheres[i].r);
                        }
                        if (ImGui::ColorEdit3("Albedo", &spheres[i].mat.albedo.x)) {
                            shader->SetVec3("u_spheres[" + std::to_string(i) + "].mat.albedo",
                                spheres[i].mat.albedo);
                        }
                        if (ImGui::ColorEdit3("Emissive", &spheres[i].mat.emissive.x)) {
                            shader->SetVec3("u_spheres[" + std::to_string(i) + "].mat.emissive",
                                spheres[i].mat.emissive);
                        }
                        if (ImGui::SliderFloat("Roughness", &spheres[i].mat.roughness, 0.0f, 1.0f)) {
                            shader->SetFloat("u_spheres[" + std::to_string(i) + "].mat.roughness",
                                spheres[i].mat.roughness);
                        }
                        if (ImGui::SliderFloat("Metallic", &spheres[i].mat.metallic, 0.0f, 1.0f)) {
                            shader->SetFloat("u_spheres[" + std::to_string(i) + "].mat.metallic",
                                spheres[i].mat.metallic);
                        }
                        if (ImGui::SliderFloat("IOR", &spheres[i].mat.ior, 0.0f, 5.0f)) {
                            shader->SetFloat("u_spheres[" + std::to_string(i) + "].mat.ior",
                                spheres[i].mat.ior);
                        }
                        if (ImGui::SliderFloat("Transparency", &spheres[i].mat.transparency, 0.0f, 1.0f)) {
                            shader->SetFloat("u_spheres[" + std::to_string(i) + "].mat.transparency",
                                spheres[i].mat.transparency);
                        }
                        ImGui::TreePop();
                    }
                }
            }

            // 3. Environment
            if (ImGui::CollapsingHeader("Environment", nodeFlags))
            {
                // Environment Light
                if (ImGui::TreeNode("Environment Light"))
                {
                    if (ImGui::ColorEdit3("Sky Color", &ambientLight.skyColor.x)) {
                        shader->SetVec3("u_ambientLight.skyColor", ambientLight.skyColor);
                    }
                    if (ImGui::ColorEdit3("Ground Color", &ambientLight.groundColor.x)) {
                        shader->SetVec3("u_ambientLight.groundColor", ambientLight.groundColor);
                    }
                    if (ImGui::SliderFloat("Intensity", &ambientLight.intensity, 0.0f, 10.0f)) {
                        shader->SetFloat("u_ambientLight.intensity", ambientLight.intensity);
                    }
                    ImGui::TreePop();
                }

                // Fog
                if (ImGui::TreeNode("Fog"))
                {
                    if (ImGui::Checkbox("Enabled", &fog.enabled)) {
                        shader->SetInt("u_fog.enabled", fog.enabled ? 1 : 0);
                    }
                    if (ImGui::ColorEdit3("Color", &fog.color.x)) {
                        shader->SetVec3("u_fog.color", fog.color);
                    }
                    if (ImGui::SliderFloat("Density", &fog.density, 0.0f, 5.0f)) {
                        shader->SetFloat("u_fog.density", fog.density);
                    }
                    if (ImGui::SliderFloat("Start", &fog.start, 0.0f, 10.0f)) {
                        shader->SetFloat("u_fog.start", fog.start);
                    }
                    if (ImGui::SliderFloat("End", &fog.end, 0.0f, 100.0f)) {
                        shader->SetFloat("u_fog.end", fog.end);
                    }
                    ImGui::TreePop();
                }
            }

            // 4. Point Lights
            if (ImGui::CollapsingHeader("Point Lights", nodeFlags))
            {
                if (ImGui::SliderInt("Active Lights", &numActiveLights, 0, MAX_LIGHTS)) {
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

            // 5. Rendering Parameters
            if (ImGui::CollapsingHeader("Rendering Settings", nodeFlags))
            {

                if (ImGui::SliderInt("SSAA Samples", &ssaaSamples, 1, 8)) {
                    shader->SetInt("u_SSAA", ssaaSamples);
                }
                if (ImGui::SliderInt("Max Bounces", &maxBounces, 0, 8)) {
                    shader->SetInt("u_maxBounces", maxBounces);
                }
                if (ImGui::SliderFloat("Shadow Softness", &softShadowFactor, 0.0f, 1.0f)) {
                    shader->SetFloat("u_softShadowFactor", softShadowFactor);
                }
            }

            // 6. Post-Processing
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
}
