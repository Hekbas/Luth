#include "luthpch.h"
#include "luth/window/Window.h"
#include "luth/core/Log.h"

#include <GLFW/glfw3.h>

namespace Luth
{
    static void GLFW_ErrorCallback(int error, const char* description) {
        LH_CORE_ERROR("GLFW Error ({0}): {1}", error, description);
    }

    Window::Window(const WindowSpec& spec)
        : m_Spec(spec) {
        Init();
    }

    Window::~Window() {
        Shutdown();
    }

    void Window::Init()
    {
        static bool s_GLFWInitialized = false;
        if (!s_GLFWInitialized) {
            if (!glfwInit()) {
                LH_CORE_CRITICAL("Failed to initialize GLFW!");
                return;
            }
            glfwSetErrorCallback(GLFW_ErrorCallback);
            s_GLFWInitialized = true;
        }

        GLFWmonitor* monitor = m_Spec.Fullscreen ? glfwGetPrimaryMonitor() : nullptr;

        glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 4);
        glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 6);
        glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);

        m_Window = glfwCreateWindow(
            m_Spec.Width,
            m_Spec.Height,
            m_Spec.Title.c_str(),
            monitor,
            nullptr
        );

        if (!m_Window) {
            LH_CORE_CRITICAL("Failed to create GLFW window!");
            glfwTerminate();
            return;
        }

        glfwMakeContextCurrent(m_Window);
        SetVSync(m_Spec.VSync);

        glfwSetWindowUserPointer(m_Window, this);
        glfwSetWindowSizeCallback(m_Window, [](GLFWwindow* window, int width, int height) {
            Window* win = static_cast<Window*>(glfwGetWindowUserPointer(window));
            win->m_Spec.Width = width;
            win->m_Spec.Height = height;
        });

        LH_CORE_INFO("Created window '{0}' ({1}x{2})", m_Spec.Title, m_Spec.Width, m_Spec.Height);
    }

    void Window::Shutdown()
    {
        if (m_Window) {
            glfwDestroyWindow(m_Window);
            LH_CORE_INFO("Destroyed window '{0}'", m_Spec.Title);
            m_Window = nullptr;
        }
    }

    void Window::OnUpdate()
    {
        glfwPollEvents();
    }

    void Window::SwapBuffers()
    {
        glfwSwapBuffers(m_Window);
    }

    void Window::SetVSync(bool enabled)
    {
        glfwSwapInterval(enabled ? 1 : 0);
        m_Spec.VSync = enabled;
        LH_CORE_INFO("VSync {0}", enabled ? "enabled" : "disabled");
    }

    void Window::ToggleFullscreen()
    {
        m_Spec.Fullscreen = !m_Spec.Fullscreen;

        if (m_Spec.Fullscreen)
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
                m_Spec.Width,
                m_Spec.Height,
                0
            );
        }
    }
}
