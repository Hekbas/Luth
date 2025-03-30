#include <Luth.h>
#include "VulkanApp.h"

#include <imgui.h>

// TEST
#include <luth/resources/ShaderLibrary.h>
#include <luth/resources/FileSystem.h>

#include <luth/renderer/Renderer.h>
#include <luth/renderer/Buffer.h>
#include <luth/renderer/vulkan/VKRendererAPI.h>
#include <luth/renderer/Shader.h>
#include <memory>

// TEST VULKAN
#include <GLFW/glfw3.h>
#include <glm/glm.hpp>

namespace Luth
{
    VulkanApp::VulkanApp(int argc, char** argv) : App(argc, argv) {}

    void VulkanApp::OnInit()
    {
        const std::vector<VKVertex> vertices = {
            { {-0.5f, -0.5f}, {1.0f, 0.0f, 0.0f} },
            { { 0.5f, -0.5f}, {0.0f, 1.0f, 0.0f} },
            { { 0.5f,  0.5f}, {0.0f, 0.0f, 1.0f} },
            { {-0.5f,  0.5f}, {1.0f, 1.0f, 1.0f} }
        };

        const std::vector<uint32_t> indices = {
            0, 1, 2, 2, 3, 0
        };

        BufferLayout layout = {
            { ShaderDataType::Float2, "inPosition" },
            { ShaderDataType::Float3, "inColor"    }
        };

        auto vkRenderer = static_cast<VKRendererAPI*>(Renderer::GetRendererAPI());
        auto vb = std::make_shared<VKVertexBuffer>(
            vkRenderer->GetLogicalDevice().GetHandle(),
            vkRenderer->GetPhysicalDevice().GetHandle(),
            vertices.data(),
            sizeof(VKVertex) * vertices.size(),
            vkRenderer->GetLogicalDevice().GetTransferQueue(),
            vkRenderer->GetPhysicalDevice().FindQueueFamilies(vkRenderer->GetSurface()).transferFamily.value()
        );

        vb->SetLayout(layout);

        auto ib = std::make_shared<VKIndexBuffer>(
            vkRenderer->GetLogicalDevice().GetHandle(),
            vkRenderer->GetPhysicalDevice().GetHandle(),
            indices.data(),
            indices.size(),
            vkRenderer->GetLogicalDevice().GetTransferQueue(),
            vkRenderer->GetPhysicalDevice().FindQueueFamilies(vkRenderer->GetSurface()).transferFamily.value()
        );

        auto mesh = std::make_shared<VKMesh>(vb, ib);

        Renderer::SubmitMesh(mesh);
    }

    void VulkanApp::OnUpdate()
    {
    }

    void VulkanApp::OnUIRender()
    {
        // ImGui Demo
        static bool showDemo = true;
        if (showDemo) ImGui::ShowDemoWindow(&showDemo);
    }

    void VulkanApp::OnShutdown()
    {
        //vkDestroyInstance(instance, nullptr);
    }
}
