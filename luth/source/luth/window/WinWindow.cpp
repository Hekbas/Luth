#include "luthpch.h"
#include "luth/window/WinWindow.h"
#include "luth/events/EventBus.h"
#include "luth/events/AppEvent.h"
#include "luth/events/KeyEvent.h"
#include "luth/events/MouseEvent.h"
#include "luth/events/FileDropEvent.h"
#include "luth/resources/FileSystem.h"

#ifdef _WIN32
#include <windows.h>
#include <dwmapi.h>
#pragma comment(lib, "dwmapi.lib")
#endif

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
            bool init = glfwInit();
            LH_CORE_ASSERT(init, "Failed to initialize GLFW!");
            glfwSetErrorCallback(GLFW_ErrorCallback);
            s_GLFWInitialized = true;
        }

        GLFWmonitor* monitor = spec.Fullscreen ? glfwGetPrimaryMonitor() : nullptr;

        m_GLFWwindow = glfwCreateWindow((int)spec.Width, (int)spec.Height, spec.Title.c_str(), monitor, nullptr);

        if (!m_GLFWwindow) {
            LH_CORE_CRITICAL("Failed to create GLFW window!");
            glfwTerminate();
            return;
        }

        auto path = FileSystem::ProjectPath() / "icons/image/";
		SetWindowIcon(m_GLFWwindow, path);

        glfwWindowHint(GLFW_SAMPLES, 4);

        if (spec.rendererAPI == RendererAPI::API::OpenGL) {
            glfwMakeContextCurrent(m_GLFWwindow);
            glfwSwapInterval(spec.VSync ? 1 : 0);

            glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 4);
            glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 6);
            glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
        }
        else if (spec.rendererAPI == RendererAPI::API::Vulkan) {
            glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
            glfwWindowHint(GLFW_RESIZABLE, GLFW_TRUE);
        }

        glfwSetWindowPos(m_GLFWwindow, spec.Width/2, spec.Height/2);

        glfwSetWindowUserPointer(m_GLFWwindow, &m_Data);

        glfwSetWindowSizeCallback(m_GLFWwindow, [](GLFWwindow* window, int width, int height) {
            WindowData& data = *(WindowData*)glfwGetWindowUserPointer(window);
            data.Width = width;
            data.Height = height;
            EventBus::Enqueue<WindowResizeEvent>(BusType::MainThread, width, height);
        });

        glfwSetWindowCloseCallback(m_GLFWwindow, [](GLFWwindow* window) {
            WindowData& data = *(WindowData*)glfwGetWindowUserPointer(window);
            EventBus::Enqueue<WindowCloseEvent>(BusType::MainThread);
        });

        glfwSetDropCallback(m_GLFWwindow, [](GLFWwindow* window, int count, const char** paths) {
            WindowData& data = *(WindowData*)glfwGetWindowUserPointer(window);
            std::vector<std::filesystem::path> files;
            for (int i = 0; i < count; i++) files.emplace_back(paths[i]);
            EventBus::Enqueue<FileDropEvent>(BusType::MainThread, std::move(files));
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
        if (Renderer::GetAPI() == RendererAPI::API::OpenGL) {
            glfwSwapBuffers(m_GLFWwindow);
        }
        else if (Renderer::GetAPI() == RendererAPI::API::Vulkan) {
            //LH_CORE_WARN("SwapBuffers not yet implemented for Vulkan");
        }
    }

    void WinWindow::SetVSync(bool enabled)
    {
        if (Renderer::GetAPI() == RendererAPI::API::OpenGL) {
            glfwSwapInterval(enabled ? 1 : 0);
            m_Data.VSync = enabled;
            LH_CORE_INFO("VSync {0}", enabled ? "enabled" : "disabled");
        }
        else if (Renderer::GetAPI() == RendererAPI::API::Vulkan) {
            LH_CORE_WARN("Vsync not yet implemented for Vulkan");
        }
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

    bool WinWindow::IsMinimized()
    {
        return glfwGetWindowAttrib(m_GLFWwindow, GLFW_ICONIFIED);
    }

    void WinWindow::SetWindowColors(const Vec3& caption, const Vec3& border, const Vec3& text)
    {
        HWND hwnd = glfwGetWin32Window(m_GLFWwindow);

        // Check if Windows version supports color attributes (Windows 10 build 1903+)
        if (WINVER >= 0x0A00)
        {
            COLORREF captionColor = RGB(caption.x, caption.y, caption.z);
            DwmSetWindowAttribute(hwnd, DWMWA_CAPTION_COLOR, &captionColor, sizeof(captionColor));

            COLORREF borderColor = RGB(border.x, border.y, border.z);
            DwmSetWindowAttribute(hwnd, DWMWA_BORDER_COLOR, &borderColor, sizeof(borderColor));

            COLORREF textColor = RGB(text.x, text.y, text.z);
            DwmSetWindowAttribute(hwnd, DWMWA_TEXT_COLOR, &textColor, sizeof(textColor));
        }
        else
        {
            // Older Windows version - fallback or do nothing
            // I could implement basic color changes using:
            // SetClassLongPtr(hwnd, GCLP_HBRBACKGROUND, (LONG_PTR)CreateSolidBrush(RGB(255,0,0)));
        }
    }

    void WinWindow::SetWindowIcon(GLFWwindow* window, fs::path iconPath)
    {
        // List of sizes
        std::vector<int> sizes = { 16, 24, 32, 48, 64, 96, 128, 256 };
        std::vector<GLFWimage> icons;

        // Load all icons
        for (int size : sizes) {
            std::string fullPath = iconPath.string() + "Luth" + std::to_string(size) + ".png";
            GLFWimage icon;
            icon.pixels = stbi_load(fullPath.c_str(), &icon.width, &icon.height, 0, 4);
            if (icon.pixels) { 
                icons.push_back(icon);
            }
        }

        // Set window icons
        if (!icons.empty()) {
            glfwSetWindowIcon(window, icons.size(), icons.data());
        }

        // Free memory
        for (auto& icon : icons) {
            stbi_image_free(icon.pixels);
        }
    }
}
