#include "luthpch.h"
#include "luth/editor/Editor.h"
#include "luth/core/Log.h"

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
        ImGui_ImplGlfw_InitForOpenGL(static_cast<GLFWwindow*>(window), true);
        ImGui_ImplOpenGL3_Init("#version 460");
        LH_CORE_INFO("Initialized ImGui context");
    }

    void Editor::Shutdown()
    {
        ImGui_ImplOpenGL3_Shutdown();
        ImGui_ImplGlfw_Shutdown();
        ImGui::DestroyContext();
        s_Context = nullptr;
        LH_CORE_INFO("Shutdown ImGui context");
    }

    void Editor::BeginFrame()
    {
        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplGlfw_NewFrame();
        ImGui::NewFrame();
    }

    void Editor::EndFrame()
    {
        ImGui::Render();
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
    }

    bool Editor::WantCaptureMouse()
    {
        return ImGui::GetIO().WantCaptureMouse;
        return true;
    }

    bool Editor::WantCaptureKeyboard()
    {
        return ImGui::GetIO().WantCaptureKeyboard;
    }
}
