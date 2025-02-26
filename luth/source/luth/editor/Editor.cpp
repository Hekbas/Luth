#include "luthpch.h"
#include "luth/editor/Editor.h"
#include "luth/renderer/Renderer.h"

#include <imgui.h>
#include <backends/imgui_impl_glfw.h>
#include <backends/imgui_impl_opengl3.h>

namespace Luth
{
    void Editor::Init(void* window)
    {
        IMGUI_CHECKVERSION();
        s_Context = ImGui::CreateContext();
        ImGui::StyleColorsDark();
        
        // TODO: Set Render specific Imgui Backends (GL/VK)
        if (Renderer::GetAPI() == RendererAPI::API::OpenGL) {
            ImGui_ImplGlfw_InitForOpenGL(static_cast<GLFWwindow*>(window), true);
            ImGui_ImplOpenGL3_Init("#version 460");
            LH_CORE_INFO("Initialized ImGui context for OpenGL");
        }
        else if (Renderer::GetAPI() == RendererAPI::API::Vulkan) {
            LH_CORE_WARN("ImGui not yet implemented for Vulkan");
        }
    }

    void Editor::Shutdown()
    {
        if (Renderer::GetAPI() == RendererAPI::API::OpenGL) {
            ImGui_ImplOpenGL3_Shutdown();
            ImGui_ImplGlfw_Shutdown();
            ImGui::DestroyContext();
        }
        else if (Renderer::GetAPI() == RendererAPI::API::Vulkan) {

        }
        
        s_Context = nullptr;
        LH_CORE_INFO("Shutdown ImGui context");
    }

    void Editor::BeginFrame()
    {
        if (Renderer::GetAPI() == RendererAPI::API::OpenGL) {
            ImGui_ImplOpenGL3_NewFrame();
            ImGui_ImplGlfw_NewFrame();
            ImGui::NewFrame();
        }
        else if (Renderer::GetAPI() == RendererAPI::API::Vulkan) {

        }
    }

    void Editor::EndFrame()
    {
        if (Renderer::GetAPI() == RendererAPI::API::OpenGL) {
            ImGui::Render();
            ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
        }
        else if (Renderer::GetAPI() == RendererAPI::API::Vulkan) {

        }
    }

    bool Editor::WantCaptureMouse()
    {
        return ImGui::GetIO().WantCaptureMouse;
    }

    bool Editor::WantCaptureKeyboard()
    {
        return ImGui::GetIO().WantCaptureKeyboard;
    }
}
