#include "Luthpch.h"
#include "luth/core/Window.h"
#include "luth/core/Log.h"
#include "luth/events/Event.h"
#include "luth/events/AppEvent.h"
#include "luth/events/KeyEvent.h"

namespace Luth
{
    static void GLFWErrorCallback(int error, const char* description)
    {
        LH_CORE_ERROR("GLFW Error ({0}): {1}", error, description);
    }

    Window::Window(EventBus& eventBus, const WindowSpec& spec)
        : m_EventBus(eventBus), m_Spec(spec) {
        Init();
    }

    Window::~Window()
    {
        Shutdown();
    }

    void Window::Init()
    {
        // Initialize GLFW
        if (!glfwInit()) 
        {
            LH_CORE_CRITICAL("Could not initialize GLFW!");
            return;
        }

        // Window hints
        glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 4);
        glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 6);
        glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
        glfwWindowHint(GLFW_SAMPLES, 4);

        // Create window
        m_Window = glfwCreateWindow(
            m_Spec.Width,
            m_Spec.Height,
            m_Spec.Title.c_str(),
            m_Spec.Fullscreen ? glfwGetPrimaryMonitor() : nullptr,
            nullptr
        );

        if (!m_Window)
        {
            LH_CORE_CRITICAL("Failed to create GLFW window!");
            glfwTerminate();
            return;
        }

        // Set GLFW callbacks
        glfwSetWindowUserPointer(m_Window, this);
        glfwSetErrorCallback(GLFWErrorCallback);

        // Event Callbacks (Buffered)
        glfwSetWindowSizeCallback(m_Window, [](GLFWwindow* window, int width, int height) {
            Window& win = *static_cast<Window*>(glfwGetWindowUserPointer(window));
            win.m_Spec.Width = width;
            win.m_Spec.Height = height;
            win.m_EventBus.Enqueue<WindowResizeEvent>(width, height);
        });

        glfwSetWindowCloseCallback(m_Window, [](GLFWwindow* window) {
            Window& win = *static_cast<Window*>(glfwGetWindowUserPointer(window));
            win.m_EventBus.Enqueue<WindowCloseEvent>();
        });

        glfwSetKeyCallback(m_Window, [](GLFWwindow* window, int key, int scancode, int action, int mods) {
            Window& win = *static_cast<Window*>(glfwGetWindowUserPointer(window));

            switch (action)
            {
                case GLFW_PRESS:    win.m_EventBus.Enqueue<KeyPressedEvent>(key, 0);    break;
                case GLFW_REPEAT:   win.m_EventBus.Enqueue<KeyPressedEvent>(key, 1);    break;
                case GLFW_RELEASE:  win.m_EventBus.Enqueue<KeyReleasedEvent>(key);      break;
            }
        });

        // Set context and VSync
        glfwMakeContextCurrent(m_Window);
        SetVSync(m_Spec.VSync);
    }

    void Window::Shutdown()
    {
        glfwDestroyWindow(m_Window);
        glfwTerminate();
    }

    void Window::OnUpdate()
    {
        glfwPollEvents();
        glfwSwapBuffers(m_Window);
    }

    void Window::SetVSync(bool enabled)
    {
        glfwSwapInterval(enabled ? 1 : 0);
        m_Spec.VSync = enabled;
    }

    void Window::ToggleFullscreen()
    {
        GLFWmonitor* monitor = glfwGetPrimaryMonitor();
        const GLFWvidmode* mode = glfwGetVideoMode(monitor);

        if (m_Spec.Fullscreen) {
            glfwSetWindowMonitor(m_Window, nullptr,
                100, 100, m_Spec.Width, m_Spec.Height, mode->refreshRate);
        }
        else {
            glfwSetWindowMonitor(m_Window, monitor,
                0, 0, mode->width, mode->height, mode->refreshRate);
        }

        m_Spec.Fullscreen = !m_Spec.Fullscreen;
    }
}
