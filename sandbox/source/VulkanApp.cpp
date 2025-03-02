#include <Luth.h>

#include <imgui.h>

// TEST
#include <luth/resources/ShaderLibrary.h>
#include <luth/resources/ResourceManager.h>

#include <luth/renderer/Renderer.h>
#include <luth/renderer/vulkan/VKRendererAPI.h>
#include <luth/renderer/Shader.h>
#include <memory>

// TEST VULKAN
#define GLFW_INCLUDE_VULKAN
#include <GLFW/glfw3.h>
#include <glm/glm.hpp>

namespace Luth
{
    class VulkanApp : public App
    {
    public:
        VulkanApp(int argc, char** argv) : App(argc, argv) {}
        ~VulkanApp() override = default;

    protected:
        void OnInit() override
        {

        }

        void OnUpdate(f32 dt) override
        {
            static float time = 0;
            time += dt;

        }

        void OnUIRender() override
        {
            // ImGui Demo
            static bool showDemo = true;
            if (showDemo) ImGui::ShowDemoWindow(&showDemo);
        }

        void OnShutdown() override
        {
            //vkDestroyInstance(instance, nullptr);
        }
    };
}
