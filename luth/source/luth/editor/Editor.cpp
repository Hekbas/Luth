#include "luthpch.h"
#include "luth/editor/Editor.h"
#include "luth/renderer/Renderer.h"
#include "luth/window/WinWindow.h"

#include "luth/resources/FileSystem.h"

#include "luth/editor/panels/HierarchyPanel.h"
#include "luth/editor/panels/InspectorPanel.h"
#include "luth/editor/panels/ProjectPanel.h"
#include "luth/editor/panels/ScenePanel.h"

#include <imgui.h>
#include <backends/imgui_impl_glfw.h>
#include <backends/imgui_impl_opengl3.h>

namespace Luth
{
    void Editor::Init(void* window)
    {
        LH_CORE_INFO("Initializing Luth Editor");
        IMGUI_CHECKVERSION();
        LH_CORE_TRACE(" - Initialized ImGui context for OpenGL");
        s_Context = ImGui::CreateContext();
        ImGuiIO& io = ImGui::GetIO();
        io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;
        LH_CORE_TRACE(" - Enabled ImGui docking support");
        io.ConfigFlags |= ImGuiConfigFlags_ViewportsEnable;
        LH_CORE_TRACE(" - Enabled ImGui multi-viewport support");
        
        SetCustomStyle();
        //SetBubblegumStyle();
        //SetRandomStyle();
        
        // TODO: Set Render specific Imgui Backends (GL/VK)
        if (Renderer::GetAPI() == RendererAPI::API::OpenGL) {
            LH_CORE_TRACE(" - Initialized ImGui GLFW/OpenGL3 backend");
            ImGui_ImplGlfw_InitForOpenGL(static_cast<GLFWwindow*>(window), true);
            ImGui_ImplOpenGL3_Init("#version 460");
        }
        else if (Renderer::GetAPI() == RendererAPI::API::Vulkan) {
            LH_CORE_WARN("ImGui not yet implemented for Vulkan");
        }

        // Set Panels
        AddPanel(new HierarchyPanel(new Scene));
        AddPanel(new InspectorPanel());
        AddPanel(new ProjectPanel());
        AddPanel(new ScenePanel());

        // Init all panels
        for (auto& panel : s_Panels)
            panel->OnInit();
    }

    void Editor::Shutdown()
    {
        LH_CORE_TRACE("Cleaning up {} panels", s_Panels.size());
        s_Panels.clear();

        if (Renderer::GetAPI() == RendererAPI::API::OpenGL) {
            LH_CORE_TRACE("Shutting down ImGui OpenGL backend");
            ImGui_ImplOpenGL3_Shutdown();
            ImGui_ImplGlfw_Shutdown();
            ImGui::DestroyContext();
        }
        else if (Renderer::GetAPI() == RendererAPI::API::Vulkan) {
            LH_CORE_WARN("Skipping Vulkan ImGui shutdown (not implemented)");
        }
        
        s_Context = nullptr;
        LH_CORE_INFO("Editor system shutdown completed");
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
        ImGuiIO& io = ImGui::GetIO();

        if (Renderer::GetAPI() == RendererAPI::API::OpenGL) {
            ImGui::Render();
            ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
            
            // Handle multi-viewport updates
            if (io.ConfigFlags & ImGuiConfigFlags_ViewportsEnable)
            {
                GLFWwindow* backup_current_context = glfwGetCurrentContext();
                ImGui::UpdatePlatformWindows();
                ImGui::RenderPlatformWindowsDefault();
                glfwMakeContextCurrent(backup_current_context);
            }
        }
        else if (Renderer::GetAPI() == RendererAPI::API::Vulkan) {

        }
    }

    void Editor::Render()
    {
        // Create dockspace
        static bool dockspaceOpen = true;
        static ImGuiDockNodeFlags dockspaceFlags = ImGuiDockNodeFlags_None;

        // Fullscreen parent window for dockspace
        ImGuiViewport* viewport = ImGui::GetMainViewport();
        ImGui::SetNextWindowPos(viewport->WorkPos);
        ImGui::SetNextWindowSize(viewport->WorkSize);
        ImGui::SetNextWindowViewport(viewport->ID);

        ImGuiWindowFlags hostWindowFlags =
            ImGuiWindowFlags_NoTitleBar |
            ImGuiWindowFlags_NoCollapse |
            ImGuiWindowFlags_NoResize |
            ImGuiWindowFlags_NoMove |
            ImGuiWindowFlags_NoDocking |
            ImGuiWindowFlags_NoBringToFrontOnFocus |
            ImGuiWindowFlags_NoBackground;

        ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));

        ImGui::Begin("DockSpaceHost", &dockspaceOpen, hostWindowFlags);
        ImGui::PopStyleVar(3);

        // Create dockspace
        ImGuiID dockspaceID = ImGui::GetID("MainDockSpace");
        ImGui::DockSpace(dockspaceID, ImVec2(0.0f, 0.0f), dockspaceFlags);

        // Render all panels
        for (auto& panel : s_Panels)
            panel->OnRender();

        ImGui::End();
    }

    bool Editor::WantCaptureMouse()
    {
        return ImGui::GetIO().WantCaptureMouse;
    }

    bool Editor::WantCaptureKeyboard()
    {
        return ImGui::GetIO().WantCaptureKeyboard;
    }

    void Editor::AddPanel(Panel* panel)
    {
        LH_CORE_ASSERT(panel, "Tried to add null panel");
        s_Panels.emplace_back(panel);
    }

    void Editor::SetCustomStyle()
    {
        ImGuiStyle& style = ImGui::GetStyle();
        ImVec4* colors = style.Colors;

        // Style settings
        style.WindowRounding    = 5.0f;
        style.ChildRounding     = 5.0f;
        style.FrameRounding     = 3.0f;
        style.GrabRounding      = 3.0f;
        style.PopupRounding     = 5.0f;
        style.ScrollbarRounding = 2.0f;
        style.TabRounding       = 3.0f;
        
        style.WindowBorderSize  = 0.0f;
        style.ChildBorderSize   = 0.0f;
        style.PopupBorderSize   = 0.0f;
        style.FrameBorderSize   = 0.0f;
        style.TabBorderSize     = 0.0f;

        // Padding and spacing
        style.WindowPadding     = ImVec2(8, 8);
        style.FramePadding      = ImVec2(6, 4);
        style.ItemSpacing       = ImVec2(6, 4);
        style.ItemInnerSpacing  = ImVec2(4, 4);
        style.TouchExtraPadding = ImVec2(0, 0);
        style.IndentSpacing     = 20;
        style.ScrollbarSize     = 14;
        style.GrabMinSize       = 12;

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
        colors[ImGuiCol_DockingEmptyBg]         = ImVec4(0.00f, 0.00f, 0.00f, 0.00f);
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
    }

    void Editor::SetBubblegumStyle()
    {
        // Font
        ImGuiIO& io = ImGui::GetIO();
        io.Fonts->AddFontFromFileTTF(FileSystem::GetPath(Resource::Font, "BubbleGum.ttf").string().c_str(), 16.0f);

        ImGuiStyle& style = ImGui::GetStyle();
    
        // Rounded corners
        style.WindowRounding    = 12.0f;
        style.ChildRounding     = 0.0f;
        style.FrameRounding     = 12.0f;
        style.PopupRounding     = 8.0f;
        style.ScrollbarRounding = 12.0f;
        style.GrabRounding      = 12.0f;
        style.TabRounding       = 8.0f;

        // Spacing and sizing
        style.WindowPadding     = ImVec2(12, 12);
        style.FramePadding      = ImVec2(12, 6);
        style.ItemSpacing       = ImVec2(10, 8);
        style.ScrollbarSize     = 16.0f;
        style.GrabMinSize       = 12.0f;
        //style.DockingSeparatorSize = 2.0f;

        // Colors (RGBA - 0-1 floats)
        ImVec4* colors = style.Colors;
        colors[ImGuiCol_Text]                   = ImVec4(0.25f, 0.16f, 0.29f, 1.00f);   // Dark purple text
        colors[ImGuiCol_TextDisabled]           = ImVec4(0.65f, 0.55f, 0.65f, 1.00f);   // Muted purple
        colors[ImGuiCol_WindowBg]               = ImVec4(1.00f, 0.89f, 0.93f, 1.00f);   // Pale pink
        colors[ImGuiCol_ChildBg]                = ImVec4(0.98f, 0.95f, 0.97f, 1.00f);
        colors[ImGuiCol_PopupBg]                = ImVec4(1.00f, 0.95f, 0.98f, 1.00f);
        colors[ImGuiCol_Border]                 = ImVec4(1.00f, 0.70f, 0.82f, 0.50f);   // Pink border
        colors[ImGuiCol_BorderShadow]           = ImVec4(0.00f, 0.00f, 0.00f, 0.00f);
        colors[ImGuiCol_FrameBg]                = ImVec4(1.00f, 0.96f, 0.98f, 1.00f);
        colors[ImGuiCol_FrameBgHovered]         = ImVec4(1.00f, 0.89f, 0.93f, 1.00f);
        colors[ImGuiCol_FrameBgActive]          = ImVec4(1.00f, 0.82f, 0.89f, 1.00f);
        colors[ImGuiCol_TitleBg]                = ImVec4(1.00f, 0.70f, 0.82f, 1.00f);   // Bright pink
        colors[ImGuiCol_TitleBgActive]          = ImVec4(1.00f, 0.60f, 0.75f, 1.00f);
        colors[ImGuiCol_TitleBgCollapsed]       = ImVec4(1.00f, 0.89f, 0.93f, 1.00f);
        colors[ImGuiCol_MenuBarBg]              = ImVec4(1.00f, 0.89f, 0.93f, 1.00f);
        colors[ImGuiCol_ScrollbarBg]            = ImVec4(1.00f, 0.95f, 0.97f, 1.00f);
        colors[ImGuiCol_ScrollbarGrab]          = ImVec4(1.00f, 0.70f, 0.82f, 0.50f);
        colors[ImGuiCol_ScrollbarGrabHovered]   = ImVec4(1.00f, 0.60f, 0.75f, 0.50f);
        colors[ImGuiCol_ScrollbarGrabActive]    = ImVec4(1.00f, 0.50f, 0.65f, 0.50f);
        colors[ImGuiCol_CheckMark]              = ImVec4(0.47f, 0.87f, 0.63f, 1.00f);   // Mint green
        colors[ImGuiCol_SliderGrab]             = ImVec4(1.00f, 0.70f, 0.82f, 1.00f);
        colors[ImGuiCol_SliderGrabActive]       = ImVec4(1.00f, 0.60f, 0.75f, 1.00f);
        colors[ImGuiCol_Button]                 = ImVec4(1.00f, 0.70f, 0.82f, 1.00f);   // Bubblegum pink
        colors[ImGuiCol_ButtonHovered]          = ImVec4(1.00f, 0.80f, 0.89f, 1.00f);
        colors[ImGuiCol_ButtonActive]           = ImVec4(1.00f, 0.60f, 0.75f, 1.00f);
        colors[ImGuiCol_Header]                 = ImVec4(1.00f, 0.70f, 0.82f, 1.00f);
        colors[ImGuiCol_HeaderHovered]          = ImVec4(1.00f, 0.80f, 0.89f, 1.00f);
        colors[ImGuiCol_HeaderActive]           = ImVec4(1.00f, 0.60f, 0.75f, 1.00f);
        colors[ImGuiCol_Separator]              = ImVec4(1.00f, 0.70f, 0.82f, 0.50f);
        colors[ImGuiCol_SeparatorHovered]       = ImVec4(1.00f, 0.60f, 0.75f, 0.50f);
        colors[ImGuiCol_SeparatorActive]        = ImVec4(1.00f, 0.50f, 0.65f, 0.50f);
        colors[ImGuiCol_ResizeGrip]             = ImVec4(1.00f, 0.70f, 0.82f, 0.20f);
        colors[ImGuiCol_ResizeGripHovered]      = ImVec4(1.00f, 0.60f, 0.75f, 0.67f);
        colors[ImGuiCol_ResizeGripActive]       = ImVec4(1.00f, 0.50f, 0.65f, 0.95f);
        colors[ImGuiCol_DockingPreview]         = ImVec4(1.00f, 0.70f, 0.82f, 0.70f);   // Translucent bubblegum pink
        colors[ImGuiCol_DockingEmptyBg]         = ImVec4(1.00f, 0.95f, 0.97f, 1.00f);   // Very light pink
        colors[ImGuiCol_Tab]                    = ImVec4(1.00f, 0.85f, 0.92f, 1.00f);   // Light pink (unselected)
        colors[ImGuiCol_TabHovered]             = ImVec4(1.00f, 0.92f, 0.96f, 1.00f);   // Lighter pink (hover)
        colors[ImGuiCol_TabActive]              = ImVec4(1.00f, 0.70f, 0.82f, 1.00f);   // Bubblegum pink (active)
        colors[ImGuiCol_TabUnfocused]           = ImVec4(1.00f, 0.90f, 0.94f, 1.00f);   // Very light pink (unfocused)
        colors[ImGuiCol_TabUnfocusedActive]     = ImVec4(1.00f, 0.80f, 0.88f, 1.00f);   // Light active
        colors[ImGuiCol_TabDimmed]              = ImVec4(0.95f, 0.82f, 0.89f, 1.00f);   // Slightly muted pink
        colors[ImGuiCol_TabDimmedSelected]      = ImVec4(1.00f, 0.75f, 0.85f, 1.00f);   // Brighter than dimmed
        colors[ImGuiCol_PlotLines]              = ImVec4(1.00f, 0.70f, 0.82f, 1.00f);
        colors[ImGuiCol_PlotLinesHovered]       = ImVec4(1.00f, 0.60f, 0.75f, 1.00f);
        colors[ImGuiCol_PlotHistogram]          = ImVec4(0.47f, 0.87f, 0.63f, 1.00f);
        colors[ImGuiCol_PlotHistogramHovered]   = ImVec4(0.40f, 0.80f, 0.55f, 1.00f);
        colors[ImGuiCol_TableHeaderBg]          = ImVec4(1.00f, 0.89f, 0.93f, 1.00f);
        colors[ImGuiCol_TableBorderStrong]      = ImVec4(1.00f, 0.70f, 0.82f, 1.00f);
        colors[ImGuiCol_TableBorderLight]       = ImVec4(1.00f, 0.80f, 0.89f, 1.00f);
        colors[ImGuiCol_TextSelectedBg]         = ImVec4(1.00f, 0.70f, 0.82f, 0.35f);
        colors[ImGuiCol_DragDropTarget]         = ImVec4(0.47f, 0.87f, 0.63f, 1.00f);
        colors[ImGuiCol_NavHighlight]           = ImVec4(0.47f, 0.87f, 0.63f, 0.80f);   // Mint green highlight
        colors[ImGuiCol_NavWindowingHighlight]  = ImVec4(1.00f, 0.70f, 0.82f, 0.70f);
        colors[ImGuiCol_NavWindowingDimBg]      = ImVec4(0.80f, 0.80f, 0.80f, 0.20f);
        colors[ImGuiCol_ModalWindowDimBg]       = ImVec4(0.80f, 0.80f, 0.80f, 0.35f);
    }

    void Editor::SetRandomStyle()
    {
        ImGuiStyle& style = ImGui::GetStyle();
        ImVec4* colors = style.Colors;
    
        // Seed with current time
        static std::mt19937 rng(std::chrono::system_clock::now().time_since_epoch().count());
        std::uniform_real_distribution<float> dist(0.0f, 1.0f);
        std::uniform_real_distribution<float> colorDist(0.2f, 0.8f);
        std::uniform_real_distribution<float> propDist(0.5f, 3.0f);

        // Generate random base hue
        float hue = dist(rng);
        ImVec4 baseColor = ImColor::HSV(hue, 0.7f, 0.7f);
    
        // Random style properties
        style.WindowRounding = propDist(rng);
        style.ChildRounding = propDist(rng);
        style.FrameRounding = propDist(rng);
        style.GrabRounding = propDist(rng);
        style.PopupRounding = propDist(rng);
        style.ScrollbarRounding = propDist(rng);
    
        style.WindowBorderSize = dist(rng) > 0.5f ? 1.0f : 0.0f;
        style.FrameBorderSize = dist(rng) > 0.3f ? 1.0f : 0.0f;
    
        // Random color scheme
        colors[ImGuiCol_Text]             = ImVec4(dist(rng), dist(rng), dist(rng), 1.00f);
        colors[ImGuiCol_WindowBg]         = ImColor::HSV(hue, 0.2f, 0.2f);
        colors[ImGuiCol_ChildBg]          = ImColor::HSV(hue, 0.25f, 0.25f);
        colors[ImGuiCol_PopupBg]          = ImColor::HSV(hue, 0.2f, 0.3f);
        colors[ImGuiCol_Border]           = ImColor::HSV(hue, 0.4f, 0.6f);
        colors[ImGuiCol_FrameBg]          = ImColor::HSV(hue, 0.3f, 0.3f);
        colors[ImGuiCol_FrameBgHovered]   = ImColor::HSV(hue, 0.4f, 0.4f);
        colors[ImGuiCol_FrameBgActive]    = ImColor::HSV(hue, 0.5f, 0.5f);
        colors[ImGuiCol_TitleBg]          = ImColor::HSV(hue, 0.6f, 0.3f);
        colors[ImGuiCol_TitleBgActive]    = ImColor::HSV(hue, 0.7f, 0.4f);
        colors[ImGuiCol_CheckMark]        = ImVec4(1.0f, 1.0f, 1.0f, 1.0f);
        colors[ImGuiCol_SliderGrab]       = ImColor::HSV(hue, 0.8f, 0.8f);
        colors[ImGuiCol_SliderGrabActive] = ImColor::HSV(hue, 0.9f, 0.9f);
        colors[ImGuiCol_Button]           = ImColor::HSV(hue, 0.6f, 0.4f);
        colors[ImGuiCol_ButtonHovered]    = ImColor::HSV(hue, 0.7f, 0.5f);
        colors[ImGuiCol_ButtonActive]     = ImColor::HSV(hue, 0.8f, 0.6f);
        colors[ImGuiCol_Header]           = ImColor::HSV(hue, 0.5f, 0.3f);
        colors[ImGuiCol_HeaderHovered]    = ImColor::HSV(hue, 0.6f, 0.4f);
        colors[ImGuiCol_HeaderActive]     = ImColor::HSV(hue, 0.7f, 0.5f);
    
        // Random spacing/padding
        style.WindowPadding = ImVec2(propDist(rng), propDist(rng));
        style.FramePadding = ImVec2(propDist(rng), propDist(rng));
        style.ItemSpacing = ImVec2(propDist(rng), propDist(rng));
        style.ItemInnerSpacing = ImVec2(propDist(rng), propDist(rng));

        // Random window transparency
        style.Alpha = 0.8f + dist(rng) * 0.2f;

        LH_CORE_INFO("Applied random style - Hue: {0}, WindowRounding: {1}", 
                    hue, style.WindowRounding);
    }
}
