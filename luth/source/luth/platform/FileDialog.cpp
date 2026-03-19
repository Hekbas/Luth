#include "luthpch.h"
#include "luth/platform/FileDialog.h"

#ifdef _WIN32
#define GLFW_EXPOSE_NATIVE_WIN32
#include <GLFW/glfw3.h>
#include <GLFW/glfw3native.h>
#include <commdlg.h>
#endif

namespace Luth
{
    std::optional<fs::path> FileDialog::OpenFile(const char* filter)
    {
#ifdef _WIN32
        OPENFILENAMEA ofn = {};
        char szFile[MAX_PATH] = {};

        ofn.lStructSize = sizeof(ofn);
        // Get HWND from the focused GLFW window
        GLFWwindow* glfwWin = glfwGetCurrentContext();
        ofn.hwndOwner = glfwWin ? glfwGetWin32Window(glfwWin) : nullptr;
        ofn.lpstrFile = szFile;
        ofn.nMaxFile = sizeof(szFile);
        ofn.lpstrFilter = filter;
        ofn.nFilterIndex = 1;
        ofn.Flags = OFN_PATHMUSTEXIST | OFN_FILEMUSTEXIST | OFN_NOCHANGEDIR;

        if (GetOpenFileNameA(&ofn))
            return fs::path(ofn.lpstrFile);
#endif
        return std::nullopt;
    }

    std::optional<fs::path> FileDialog::SaveFile(const char* filter)
    {
#ifdef _WIN32
        OPENFILENAMEA ofn = {};
        char szFile[MAX_PATH] = {};

        ofn.lStructSize = sizeof(ofn);
        GLFWwindow* glfwWin = glfwGetCurrentContext();
        ofn.hwndOwner = glfwWin ? glfwGetWin32Window(glfwWin) : nullptr;
        ofn.lpstrFile = szFile;
        ofn.nMaxFile = sizeof(szFile);
        ofn.lpstrFilter = filter;
        ofn.nFilterIndex = 1;
        ofn.lpstrDefExt = "luth";
        ofn.Flags = OFN_OVERWRITEPROMPT | OFN_NOCHANGEDIR;

        if (GetSaveFileNameA(&ofn))
            return fs::path(ofn.lpstrFile);
#endif
        return std::nullopt;
    }
}
