#include <Luth.h>
#include "RTShaderApp.h"

#include <imgui.h>
#include <memory>

// TEST GL
#include <GLFW/glfw3.h>
#include <glad/glad.h>

namespace Luth
{
    RTShaderApp::RTShaderApp(int argc, char** argv) : App(argc, argv) {}

    void RTShaderApp::OnInit()
    {
        InitScreenQuad();
        SetVariables();
        LoadShader();
    }

    void RTShaderApp::OnUpdate()
    {
        UpdateUniforms(Time::GetTime());
    }

    void RTShaderApp::OnUIRender()
    {
        // ImGui Demo
        static bool showDemo = false;
        ShaderControls();
        if (showDemo) ImGui::ShowDemoWindow(&showDemo);

        ImGuiIO& io = ImGui::GetIO();
        ImGui::Begin("Luth Metrics");
        ImGui::Text("Frame time %.3f ms (%.1f FPS)", 1000.0f / io.Framerate, io.Framerate);
        ImGui::End();
    }

    void RTShaderApp::OnShutdown()
    {
        //vkDestroyInstance(instance, nullptr);
    }

    // Shader
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
        std::filesystem::path shaderPath = FileSystem::GetPath(Resource::Shader, "raytracing.glsl");
        shader = Shader::Create(shaderPath.generic_string());
        shader->Bind();
        InitUniforms();
    }

    void RTShaderApp::SetVariables()
    {
        // Camera
        camera.position = Vec3(0.0, 6.0, 0.0);
        camera.target = Vec3(0.0, 3.5, 0.0);
        camera.fov = 90.0f;
        camera.orbitRadius = 9.5f;
        camera.orbitSpeed = 0.3f;

        // Floor
        floorMaterial.albedo = Vec3(1.0f, 1.0f, 1.0f);
        floorMaterial.roughness = 0.8f;
        floorMaterial.metallic = 0.1f;
        floorMaterial.ior = 1.0f;
        floorMaterial.transparency = 0.0f;

        // Sphere Positions
        spheres[0].position = CalculatePosition(6, 1, 2.5, 2);
        spheres[1].position = CalculatePosition(6, 3, 2.5, 2);
        spheres[2].position = CalculatePosition(6, 5, 2.5, 2);
        spheres[3].position = CalculatePosition(3, 3, 2.5, 5);
        spheres[4].position = CalculatePosition(3, 4, 2.5, 5);
        spheres[5].position = CalculatePosition(3, 5, 2.5, 5);

        // Sphere Radius
        spheres[0].r = 1.0f;
        spheres[1].r = 1.25f;
        spheres[2].r = 0.8f;
        spheres[3].r = 1.2f;
        spheres[4].r = 0.9f;
        spheres[5].r = 0.8f;

        // Sphere Materials
        spheres[0].mat.albedo = Vec3(0.0f, 0.0f, 0.0f);
        spheres[0].mat.emissive = Vec3(0.0f, 0.0f, 0.0f);
        spheres[0].mat.roughness = 0.9;
        spheres[0].mat.metallic = 0.5;
        spheres[0].mat.ior = 1.5;
        spheres[0].mat.transparency = 0.0;

        spheres[1].mat.albedo = Vec3(0.0f, 0.0f, 1.0f);
        spheres[1].mat.emissive = Vec3(0.0f, 0.0f, 0.0f);
        spheres[1].mat.roughness = 0.035;
        spheres[1].mat.metallic = 0.9;
        spheres[1].mat.ior = 1.5;
        spheres[1].mat.transparency = 0.0;

        spheres[2].mat.albedo = Vec3(0.87f, 0.06f, 0.06f);
        spheres[2].mat.emissive = Vec3(0.0f, 0.0f, 0.0f);
        spheres[2].mat.roughness = 0.05;
        spheres[2].mat.metallic = 0.05;
        spheres[2].mat.ior = 1.5;
        spheres[2].mat.transparency = 0.0;

        spheres[3].mat.albedo = Vec3(0.0f, 1.0f, 0.0f);
        spheres[3].mat.emissive = Vec3(0.0f, 0.0f, 0.0f);
        spheres[3].mat.roughness = 0.05;
        spheres[3].mat.metallic = 1.0;
        spheres[3].mat.ior = 1.5;
        spheres[3].mat.transparency = 0.95;

        spheres[4].mat.albedo = Vec3(1.0f, 1.0f, 1.0f);
        spheres[4].mat.emissive = Vec3(0.0f, 0.0f, 0.0f);
        spheres[4].mat.roughness = 0.055;
        spheres[4].mat.metallic = 0.9;
        spheres[4].mat.ior = 0.98;
        spheres[4].mat.transparency = 1.0;

        spheres[5].mat.albedo = Vec3(1.0f, 0.0f, 0.0f);
        spheres[5].mat.emissive = Vec3(0.1f, 0.8f, 0.35f);
        spheres[5].mat.roughness = 0.05;
        spheres[5].mat.metallic = 0.05;
        spheres[5].mat.ior = 1.5;
        spheres[5].mat.transparency = 0.0;

        // Environment Light
        ambientLight.skyColor = Vec3(0.0, 0.0, 0.36);
        ambientLight.groundColor = Vec3(1.0, 0.42, 0.0);
        ambientLight.intensity = 1.0f;

        // Environment Cloud
        cloud.color = Vec3(0.75, 0.75, 0.75);
        cloud.density = 1.0;
        cloud.scale = 4.0;
        cloud.speed = 0.15;

        // Environment Fog
        fog.enabled = false;
        fog.color = Vec3(0.75, 0.75, 0.75);
        fog.density = 1.0;
        fog.start = 5.0;
        fog.end = 50.0;

        // Point Lights
        for (size_t i = 0; i < MAX_LIGHTS; i++)
        {
            pointLights[i].position = Vec3(0.0f, 3.5f, 0.0f);
            pointLights[i].color = Vec3(1.0f);
            pointLights[i].intensity = 10.0f;
        }
    }

    void RTShaderApp::InitUniforms()
    {
        // Display Mode
        shader->SetInt("u_displayMode", displayMode);

        // Camera
        shader->SetVec3("u_camera.position", camera.position);
        shader->SetVec3("u_camera.target", camera.target);
        shader->SetFloat("u_camera.fov", camera.fov);
        shader->SetFloat("u_camera.orbitRadius", camera.orbitRadius);
        shader->SetFloat("u_camera.orbitSpeed", camera.orbitSpeed);

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

        // Environment Fog
        shader->SetBool("u_fog.enabled", fog.enabled);
        shader->SetVec3("u_fog.color", fog.color);
        shader->SetFloat("u_fog.density", fog.density);
        shader->SetFloat("u_fog.start", fog.start);
        shader->SetFloat("u_fog.end", fog.end);

        // Environment Clouds
        shader->SetVec3("u_cloud.color", cloud.color);
        shader->SetFloat("u_cloud.density", cloud.density);
        shader->SetFloat("u_cloud.scale", cloud.scale);
        shader->SetFloat("u_cloud.speed", cloud.speed);

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
            // 0. Reload Shader, Reset Uniforms
            ExecuteOnButtonPress("Reload Shader", [this]() { LoadShader(); });
            ImGui::SameLine();
            ExecuteOnButtonPress("Reset Uniforms", [this]() { SetVariables(); InitUniforms(); });

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

            // 2. Camera
            if (ImGui::CollapsingHeader("Camera", nodeFlags))
            {
                if (ImGui::SliderFloat3("Position", &camera.position.x, -10.0f, 10.0f)) {
                    shader->SetVec3("u_camera.position", camera.position);
                }
                if (ImGui::SliderFloat3("Look At", &camera.target.x, -10.0f, 10.0f)) {
                    shader->SetVec3("u_camera.target", camera.target);
                }
                if (ImGui::SliderFloat("FOV", &camera.fov, 10.0f, 140.0f)) {
                    shader->SetFloat("u_camera.fov", camera.fov);
                }
                if (ImGui::SliderFloat("Orbit Radius", &camera.orbitRadius, 1.0f, 50.0f)) {
                    shader->SetFloat("u_camera.orbitRadius", camera.orbitRadius);
                }
                if (ImGui::SliderFloat("Orbit Speed", &camera.orbitSpeed, 0.0f, 5.0f)) {
                    shader->SetFloat("u_camera.orbitSpeed", camera.orbitSpeed);
                }
            }

            // 3. Scene 
            if (ImGui::CollapsingHeader("Scene", nodeFlags))
            {
                // Floor
                if (ImGui::TreeNode("Floor")) {
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

            // 4. Environment
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

                // Cloud
                if (ImGui::TreeNode("Clouds"))
                {
                    if (ImGui::ColorEdit3("Color", &cloud.color.x)) {
                        shader->SetVec3("u_cloud.color", cloud.color);
                    }
                    if (ImGui::SliderFloat("Density", &cloud.density, 0.0f, 5.0f)) {
                        shader->SetFloat("u_cloud.density", cloud.density);
                    }
                    if (ImGui::SliderFloat("Scale", &cloud.scale, 0.0f, 10.0f)) {
                        shader->SetFloat("u_cloud.scale", cloud.scale);
                    }
                    if (ImGui::SliderFloat("Speed", &cloud.speed, -5.0f, 5.0f)) {
                        shader->SetFloat("u_cloud.speed", cloud.speed);
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
                    /*if (ImGui::SliderFloat("Start", &fog.start, 0.0f, 10.0f)) {
                        shader->SetFloat("u_fog.start", fog.start);
                    }*/
                    if (ImGui::SliderFloat("End", &fog.end, 0.0f, 100.0f)) {
                        shader->SetFloat("u_fog.end", fog.end);
                    }
                    ImGui::TreePop();
                }
            }

            // 5. Point Lights
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

            // 6. Rendering Parameters
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

            // 7. Post-Processing
            if (ImGui::CollapsingHeader("Post-Processing", nodeFlags))
            {
                // Tonemapping row
                if (ImGui::Checkbox("##Tonemapping", &applyTonemap)) {
                    shader->SetInt("u_applyTonemapping", applyTonemap ? 1 : 0);
                }
                ImGui::SameLine();
                if (ImGui::SliderFloat("Exposure", &exposure, 0.1f, 5.0f)) {
                    shader->SetFloat("u_exposure", exposure);
                }

                // Gamma row
                if (ImGui::Checkbox("##Gamma Correction", &applyGamma)) {
                    shader->SetInt("u_applyGamma", applyGamma ? 1 : 0);
                }
                ImGui::SameLine();
                if (ImGui::SliderFloat("Gamma", &gammaValue, 1.0f, 3.0f)) {
                    shader->SetFloat("u_gamma", gammaValue);
                }
            }
        }

        ImGui::End();
    }
}
