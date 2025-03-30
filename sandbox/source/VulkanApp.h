#pragma once

#include <Luth.h>

#include <imgui.h>

// TEST
#include <luth/resources/ShaderLibrary.h>
#include <luth/resources/FileSystem.h>

#include <luth/renderer/Renderer.h>
#include <luth/renderer/Buffer.h>
#include <luth/renderer/Shader.h>
#include <luth/renderer/vulkan/VKRendererAPI.h>
#include <luth/renderer/vulkan/VKBuffer.h>
#include <memory>

// TEST VULKAN
#include <GLFW/glfw3.h>
#include <glm/glm.hpp>

namespace Luth
{
    struct VKVertex {
        glm::vec2 pos;
        glm::vec3 color;
    };

    class VulkanApp : public App
    {
    public:
        VulkanApp(int argc, char** argv);
        ~VulkanApp() override = default;

    protected:
        void OnInit() override;
        void OnUpdate() override;
        void OnUIRender() override;
        void OnShutdown() override;
    };
}
