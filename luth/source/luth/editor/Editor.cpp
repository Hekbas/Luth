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
        SetCustomStyle();
        
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

    void Editor::SetCustomStyle()
    {
        ImGuiStyle& style = ImGui::GetStyle();
        ImVec4* colors = style.Colors;

        // Style settings
        style.WindowRounding = 5.0f;
        style.ChildRounding = 5.0f;
        style.FrameRounding = 3.0f;
        style.GrabRounding = 3.0f;
        style.PopupRounding = 5.0f;
        style.ScrollbarRounding = 2.0f;
        style.TabRounding = 3.0f;
        
        style.WindowBorderSize = 0.0f;
        style.ChildBorderSize = 0.0f;
        style.PopupBorderSize = 0.0f;
        style.FrameBorderSize = 0.0f;
        style.TabBorderSize = 0.0f;

        // Alpha settings
        style.Alpha = 0.95f;
        style.WindowMenuButtonPosition = ImGuiDir_None;

        // Color config
        colors[ImGuiCol_Text]                   = ImVec4(0.90f, 0.90f, 0.90f, 1.00f);
        colors[ImGuiCol_TextDisabled]           = ImVec4(0.60f, 0.60f, 0.60f, 1.00f);
        colors[ImGuiCol_WindowBg]               = ImVec4(0.12f, 0.12f, 0.12f, 0.94f);
        colors[ImGuiCol_ChildBg]                = ImVec4(0.15f, 0.15f, 0.15f, 0.90f);
        colors[ImGuiCol_PopupBg]                = ImVec4(0.11f, 0.11f, 0.11f, 0.94f);
        colors[ImGuiCol_Border]                 = ImVec4(0.25f, 0.25f, 0.25f, 0.50f);
        colors[ImGuiCol_BorderShadow]           = ImVec4(0.00f, 0.00f, 0.00f, 0.00f);
        colors[ImGuiCol_FrameBg]                = ImVec4(0.20f, 0.20f, 0.20f, 0.80f);
        colors[ImGuiCol_FrameBgHovered]         = ImVec4(0.25f, 0.25f, 0.25f, 0.80f);
        colors[ImGuiCol_FrameBgActive]          = ImVec4(0.30f, 0.30f, 0.30f, 0.80f);
        colors[ImGuiCol_TitleBg]                = ImVec4(0.10f, 0.10f, 0.10f, 0.85f);
        colors[ImGuiCol_TitleBgActive]          = ImVec4(0.12f, 0.12f, 0.12f, 0.90f);
        colors[ImGuiCol_TitleBgCollapsed]       = ImVec4(0.10f, 0.10f, 0.10f, 0.60f);
        colors[ImGuiCol_MenuBarBg]              = ImVec4(0.15f, 0.15f, 0.15f, 0.90f);
        colors[ImGuiCol_ScrollbarBg]            = ImVec4(0.10f, 0.10f, 0.10f, 0.60f);
        colors[ImGuiCol_ScrollbarGrab]          = ImVec4(0.40f, 0.40f, 0.40f, 0.60f);
        colors[ImGuiCol_ScrollbarGrabHovered]   = ImVec4(0.50f, 0.50f, 0.50f, 0.60f);
        colors[ImGuiCol_ScrollbarGrabActive]    = ImVec4(0.60f, 0.60f, 0.60f, 0.60f);
        colors[ImGuiCol_CheckMark]              = ImVec4(0.90f, 0.90f, 0.90f, 0.90f);
        colors[ImGuiCol_SliderGrab]             = ImVec4(0.70f, 0.70f, 0.70f, 0.60f);
        colors[ImGuiCol_SliderGrabActive]       = ImVec4(0.85f, 0.85f, 0.85f, 0.60f);
        colors[ImGuiCol_Button]                 = ImVec4(0.20f, 0.20f, 0.20f, 0.80f);
        colors[ImGuiCol_ButtonHovered]          = ImVec4(0.30f, 0.30f, 0.30f, 0.80f);
        colors[ImGuiCol_ButtonActive]           = ImVec4(0.35f, 0.35f, 0.35f, 0.80f);
        colors[ImGuiCol_Header]                 = ImVec4(0.25f, 0.25f, 0.25f, 0.80f);
        colors[ImGuiCol_HeaderHovered]          = ImVec4(0.30f, 0.30f, 0.30f, 0.80f);
        colors[ImGuiCol_HeaderActive]           = ImVec4(0.35f, 0.35f, 0.35f, 0.80f);
        colors[ImGuiCol_Separator]              = ImVec4(0.30f, 0.30f, 0.30f, 0.60f);
        colors[ImGuiCol_SeparatorHovered]       = ImVec4(0.40f, 0.40f, 0.40f, 0.78f);
        colors[ImGuiCol_SeparatorActive]        = ImVec4(0.50f, 0.50f, 0.50f, 1.00f);
        colors[ImGuiCol_ResizeGrip]             = ImVec4(0.40f, 0.40f, 0.40f, 0.60f);
        colors[ImGuiCol_ResizeGripHovered]      = ImVec4(0.60f, 0.60f, 0.60f, 0.60f);
        colors[ImGuiCol_ResizeGripActive]       = ImVec4(0.80f, 0.80f, 0.80f, 0.60f);
        colors[ImGuiCol_Tab]                    = ImVec4(0.15f, 0.15f, 0.15f, 0.86f);
        colors[ImGuiCol_TabHovered]             = ImVec4(0.25f, 0.25f, 0.25f, 0.86f);
        colors[ImGuiCol_TabActive]              = ImVec4(0.20f, 0.20f, 0.20f, 0.86f);
        colors[ImGuiCol_TabUnfocused]           = ImVec4(0.15f, 0.15f, 0.15f, 0.86f);
        colors[ImGuiCol_TabUnfocusedActive]     = ImVec4(0.18f, 0.18f, 0.18f, 0.86f);
        colors[ImGuiCol_DockingPreview]         = ImVec4(0.90f, 0.90f, 0.90f, 0.70f);
        colors[ImGuiCol_DockingEmptyBg]         = ImVec4(0.20f, 0.20f, 0.20f, 1.00f);
        colors[ImGuiCol_PlotLines]              = ImVec4(0.90f, 0.90f, 0.90f, 1.00f);
        colors[ImGuiCol_PlotLinesHovered]       = ImVec4(1.00f, 0.43f, 0.35f, 1.00f);
        colors[ImGuiCol_PlotHistogram]          = ImVec4(0.90f, 0.70f, 0.00f, 1.00f);
        colors[ImGuiCol_PlotHistogramHovered]   = ImVec4(1.00f, 0.60f, 0.00f, 1.00f);
        colors[ImGuiCol_TableHeaderBg]          = ImVec4(0.19f, 0.19f, 0.20f, 1.00f);
        colors[ImGuiCol_TableBorderStrong]      = ImVec4(0.31f, 0.31f, 0.35f, 1.00f);
        colors[ImGuiCol_TableBorderLight]       = ImVec4(0.23f, 0.23f, 0.25f, 1.00f);
        colors[ImGuiCol_TextSelectedBg]         = ImVec4(0.25f, 0.50f, 0.75f, 0.50f);
        colors[ImGuiCol_DragDropTarget]         = ImVec4(1.00f, 1.00f, 0.00f, 0.90f);
        colors[ImGuiCol_NavHighlight]           = ImVec4(0.45f, 0.45f, 0.90f, 0.80f);
        colors[ImGuiCol_NavWindowingHighlight]  = ImVec4(1.00f, 1.00f, 1.00f, 0.70f);
        colors[ImGuiCol_NavWindowingDimBg]      = ImVec4(0.80f, 0.80f, 0.80f, 0.20f);
        colors[ImGuiCol_ModalWindowDimBg]       = ImVec4(0.20f, 0.20f, 0.20f, 0.35f);

        // Padding and spacing
        style.WindowPadding = ImVec2(8, 8);
        style.FramePadding = ImVec2(6, 4);
        style.ItemSpacing = ImVec2(6, 4);
        style.ItemInnerSpacing = ImVec2(4, 4);
        style.TouchExtraPadding = ImVec2(0, 0);
        style.IndentSpacing = 20;
        style.ScrollbarSize = 14;
        style.GrabMinSize = 12;
    }
}
