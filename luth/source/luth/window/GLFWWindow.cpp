#include "luthpch.h"
#include "luth/window/GLFWWindow.h"


namespace Luth
{
    static void GLFW_ErrorCallback(int error, const char* description) {
        LH_CORE_ERROR("GLFW Error ({0}): {1}", error, description);
    }

    GLFWWindow::GLFWWindow(const WindowSpec& spec)
    {
        Init(spec);
    }

    GLFWWindow::~GLFWWindow()
    {
        Shutdown();
    }

    void GLFWWindow::Init(const WindowSpec& spec)
    {
        m_Data.Title = spec.Title;
        m_Data.Width = spec.Width;
        m_Data.Height = spec.Height;

        static bool s_GLFWInitialized = false;
        if (!s_GLFWInitialized) {
            if (!glfwInit()) {
                LH_CORE_CRITICAL("Failed to initialize GLFW!");
                return;
            }
            glfwSetErrorCallback(GLFW_ErrorCallback);
            s_GLFWInitialized = true;
        }

        GLFWmonitor* monitor = spec.Fullscreen ? glfwGetPrimaryMonitor() : nullptr;

        glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 4);
        glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 6);
        glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);

        m_Window = glfwCreateWindow(
            (int)spec.Width,
            (int)spec.Height,
            spec.Title.c_str(),
            monitor,
            nullptr
        );

        if (!m_Window) {
            LH_CORE_CRITICAL("Failed to create GLFW window!");
            glfwTerminate();
            return;
        }

        glfwMakeContextCurrent(m_Window);
        SetVSync(spec.VSync);

        glfwSetWindowUserPointer(m_Window, this);
        glfwSetWindowSizeCallback(m_Window, [](GLFWwindow* window, int width, int height) {
            WindowData* win = static_cast<WindowData*>(glfwGetWindowUserPointer(window));
            win->Width = width;
            win->Height = height;
        });

        LH_CORE_INFO("Created window '{0}' ({1}x{2})", spec.Title, spec.Width, spec.Height);

        m_Renderer = Renderer::Create();
        m_Renderer->Init();
    }

    void GLFWWindow::Shutdown()
    {
        if (m_Renderer) {
            m_Renderer->Shutdown();
        }

        if (m_Window) {
            glfwDestroyWindow(m_Window);
            LH_CORE_INFO("Destroyed window '{0}'", m_Data.Title);
            m_Window = nullptr;
        }
    }

    void GLFWWindow::OnUpdate()
    {
        m_Renderer->Clear();
        glfwPollEvents();
    }

    void GLFWWindow::SwapBuffers()
    {
        glfwSwapBuffers(m_Window);
    }

    void GLFWWindow::SetVSync(bool enabled)
    {
        glfwSwapInterval(enabled ? 1 : 0);
        m_Data.VSync = enabled;
        LH_CORE_INFO("VSync {0}", enabled ? "enabled" : "disabled");
    }

    void GLFWWindow::ToggleFullscreen()
    {
        m_Data.Fullscreen = !m_Data.Fullscreen;

        if (m_Data.Fullscreen)
        {
            GLFWmonitor* monitor = glfwGetPrimaryMonitor();
            const GLFWvidmode* mode = glfwGetVideoMode(monitor);
            glfwSetWindowMonitor(
                m_Window,
                monitor,
                0, 0,
                mode->width,
                mode->height,
                mode->refreshRate
            );
        }
        else
        {
            glfwSetWindowMonitor(
                m_Window,
                nullptr,
                100, 100, // Default position
                (int)m_Data.Width,
                (int)m_Data.Height,
                0
            );
        }
    }
}
