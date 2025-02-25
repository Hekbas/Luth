#include "luthpch.h"
#include "luth/window/WinWindow.h"


namespace Luth
{
    static void GLFW_ErrorCallback(int error, const char* description) {
        LH_CORE_ERROR("GLFW Error ({0}): {1}", error, description);
    }

    WinWindow::WinWindow(const WindowSpec& spec)
    {
        Init(spec);
    }

    WinWindow::~WinWindow()
    {
        Shutdown();
    }

    void WinWindow::Init(const WindowSpec& spec)
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

        //glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 4);
        //glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 6);
        //glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
        glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
        glfwWindowHint(GLFW_RESIZABLE, GLFW_TRUE);

        m_GLFWwindow = glfwCreateWindow(
            (int)spec.Width,
            (int)spec.Height,
            spec.Title.c_str(),
            monitor,
            nullptr
        );

        if (!m_GLFWwindow) {
            LH_CORE_CRITICAL("Failed to create GLFW window!");
            glfwTerminate();
            return;
        }

        // TODO: Relocate OpenGL specific stuff
        //glfwMakeContextCurrent(m_GLFWwindow);
        //SetVSync(spec.VSync);

        glfwSetWindowUserPointer(m_GLFWwindow, this);
        glfwSetWindowSizeCallback(m_GLFWwindow, [](GLFWwindow* window, int width, int height) {
            WindowData* win = static_cast<WindowData*>(glfwGetWindowUserPointer(window));
            win->Width = width;
            win->Height = height;
        });

        LH_CORE_INFO("Created window '{0}' ({1}x{2})", spec.Title, spec.Width, spec.Height);
    }

    void WinWindow::Shutdown()
    {
        if (m_GLFWwindow) {
            glfwDestroyWindow(m_GLFWwindow);
            LH_CORE_INFO("Destroyed window '{0}'", m_Data.Title);
            m_GLFWwindow = nullptr;
        }
    }

    void WinWindow::OnUpdate()
    {
        glfwPollEvents();
    }

    void WinWindow::SwapBuffers()
    {
        glfwSwapBuffers(m_GLFWwindow);
    }

    void WinWindow::SetVSync(bool enabled)
    {
        glfwSwapInterval(enabled ? 1 : 0);
        m_Data.VSync = enabled;
        LH_CORE_INFO("VSync {0}", enabled ? "enabled" : "disabled");
    }

    void WinWindow::ToggleFullscreen()
    {
        m_Data.Fullscreen = !m_Data.Fullscreen;

        if (m_Data.Fullscreen) {
            GLFWmonitor* monitor = glfwGetPrimaryMonitor();
            const GLFWvidmode* mode = glfwGetVideoMode(monitor);
            glfwSetWindowMonitor(m_GLFWwindow, monitor, 0, 0, mode->width, mode->height, mode->refreshRate);
        }
        else {
            glfwSetWindowMonitor(m_GLFWwindow, nullptr, 100, 100, (int)m_Data.Width, (int)m_Data.Height, 0);
        }
    }
}
