#include "luthpch.h"
#include "luth/ui/UI.h"
#include "luth/core/Log.h"

#include <imgui.h>
#include <backends/imgui_impl_glfw.h>
#include <backends/imgui_impl_opengl3.h>

namespace Luth
{
    void UI::Init(void* window)
    {
        IMGUI_CHECKVERSION();
        s_Context = ImGui::CreateContext();
        ImGui::StyleColorsDark();
        
        ImGui_ImplGlfw_InitForOpenGL(static_cast<GLFWwindow*>(window), true);
        ImGui_ImplOpenGL3_Init("#version 460");
        LH_CORE_INFO("Initialized ImGui context");
    }

    void UI::Shutdown()
    {
        ImGui_ImplOpenGL3_Shutdown();
        ImGui_ImplGlfw_Shutdown();
        ImGui::DestroyContext();
        s_Context = nullptr;
        LH_CORE_INFO("Shutdown ImGui context");
    }

    void UI::BeginFrame()
    {
        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplGlfw_NewFrame();
        ImGui::NewFrame();
    }

    void UI::EndFrame()
    {
        ImGui::Render();
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
    }

    bool UI::WantCaptureMouse()
    {
        return ImGui::GetIO().WantCaptureMouse;
        return true;
    }

    bool UI::WantCaptureKeyboard()
    {
        return ImGui::GetIO().WantCaptureKeyboard;
    }
}
