#include "luthpch.h"
#include "luth/platform/FileDialog.h"

#ifdef _WIN32
#define GLFW_EXPOSE_NATIVE_WIN32
#include <GLFW/glfw3.h>
#include <GLFW/glfw3native.h>
#include <commdlg.h>
#elif defined(__linux__)
#include <cstdio>
#include <cstdlib>
#include <array>
#endif

namespace Luth
{
#ifdef __linux__
    struct ParsedFilter 
    {
        std::string_view name;
        std::string_view extensions; 
    };

    static std::vector<ParsedFilter> ParseWin32Filter(const char* filter)
    {
        std::vector<ParsedFilter> filters;
        if (!filter || filter[0] == '\0') return filters;
        
        const char* p = filter;
        while (*p) 
        {
            std::string_view name(p);
            p += name.size() + 1;

            if (!*p) break;
            std::string_view ext(p);
            p += ext.size() + 1;

            filters.push_back({name, ext});
        }
        return filters;
    }

    static std::string EscapeShell(std::string_view input)
    {
        std::string out;
        for (char c : input)
        {
            if (c == '\'')
                out += "'\\''";
            else
                out += c;
        }
        return out;
    }

    static std::string BuildZenityFilters(const std::vector<ParsedFilter>& filters)
    {
        std::string result;
        for (const auto& f : filters) 
        {
            result += " --file-filter='";
            result += EscapeShell(f.name);
            result += " | ";
            for (char c : f.extensions) 
            {
                result += (c == ';') ? ' ' : c;
            }
            result += "'";
        }
        return result;
    }

    static std::string BuildKDialogFilters(const std::vector<ParsedFilter>& filters)
    {
        std::string result;
        for (const auto& f : filters) 
        {
            result += "\"";
            for (char c : f.extensions) 
            {
                result += (c == ';') ? ' ' : c;
            }
            result += " | ";
            result += EscapeShell(f.name);
            result += "\" ";
        }
        return result;
    }

    static std::string_view GetAvailableDialogTool()
    {
        static std::string tool;
        if(!tool.empty()) 
            return tool;

        const char* desktop = std::getenv("XDG_CURRENT_DESKTOP");
        bool preferKde = false;
        if (desktop) 
        {
            std::string_view d(desktop);
            if (d.find("KDE") != std::string_view::npos)
                preferKde = true;
        }

        auto checkExists = [](std::string_view cmd) -> bool {
            std::string testCmd = "command -v ";
            testCmd += cmd;
            testCmd += " 2>/dev/null";
            return std::system(testCmd.c_str()) == 0;
        };

        if (preferKde && checkExists("kdialog")) tool = "kdialog";
        if (checkExists("zenity")) tool = "zenity";
        if (checkExists("kdialog")) tool = "kdialog";
        
        return tool;
    }

    static std::string ExecCommand(const std::string& command)
    {
        std::array<char, 512> buffer;
        std::string result;
        
        FILE* pipe = popen(command.c_str(), "r");
        if (!pipe) return "";
        
        while (fgets(buffer.data(), buffer.size(), pipe) != nullptr)
            result += buffer.data();
        
        int status = pclose(pipe);
        if (status == -1 || !WIFEXITED(status) || WEXITSTATUS(status) != 0) 
            return "";
        
        while (!result.empty() && (result.back() == '\n' || result.back() == '\r'))
            result.pop_back();
            
        return result;
    }
#endif

    std::optional<fs::path> FileDialog::OpenFile(const char* filter)
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
        ofn.Flags = OFN_PATHMUSTEXIST | OFN_FILEMUSTEXIST | OFN_NOCHANGEDIR;

        if (GetOpenFileNameA(&ofn))
            return fs::path(ofn.lpstrFile);
#elif defined(__linux__)
        std::string_view tool = GetAvailableDialogTool();
        if (tool.empty()) 
            return std::nullopt;

        auto filters = ParseWin32Filter(filter);
        std::string cmd;

        if (tool == "zenity") 
        {
            cmd = "zenity --file-selection --title=\"Open File\"";
            cmd += BuildZenityFilters(filters);
        } 
        else 
        {
            cmd = "kdialog --getopenfilename . ";
            cmd += BuildKDialogFilters(filters);
            cmd += "--title \"Open File\"";
        }
        
        std::string result = ExecCommand(cmd);
        if (!result.empty())
            return fs::path(result);
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
#elif defined(__linux__)
        std::string_view tool = GetAvailableDialogTool();
        if (tool.empty()) 
            return std::nullopt;

        auto filters = ParseWin32Filter(filter);
        std::string cmd;

        if (tool == "zenity") 
        {
            cmd = "zenity --file-selection --save --confirm-overwrite --title=\"Save File\"";
            cmd += BuildZenityFilters(filters);
        } 
        else 
        {
            cmd = "kdialog --getsavefilename . ";
            cmd += BuildKDialogFilters(filters);
            cmd += "--title \"Save File\"";
        }
        
        std::string result = ExecCommand(cmd);
        if (!result.empty())
            return fs::path(result);
#endif
        return std::nullopt;
    }
}