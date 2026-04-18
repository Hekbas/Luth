#pragma once

#include <filesystem>
#include <string>
#include <vector>

namespace Luth
{
    struct RecentProject
    {
        std::string Name;
        std::string Path;        // Absolute path to the .luthproj file
        std::string Version;     // From .luthproj
        int64_t     LastOpened;  // Unix timestamp (seconds since epoch)
    };

    /// Startup project launcher — shows when no project is auto-discovered.
    /// Also accessible from File > Open Project / New Project.
    class ProjectLauncher
    {
    public:
        static void Init();

        static void Show();
        static void Hide();
        static bool IsVisible();

        /// Render the launcher UI. Call from Editor::Render().
        static void Render();

        /// Check if the user selected a project (consumed by App).
        static bool HasPendingProject();
        static std::filesystem::path ConsumePendingProject();

        /// Track recently opened projects.
        static void AddRecent(const std::string& name, const std::filesystem::path& luthprojPath);

        /// Directly signal a project switch (used by File > Open Project menu).
        static void SetPendingProject(const std::filesystem::path& luthprojPath);

    private:
        static void LoadRecentProjects();
        static void SaveRecentProjects();
        static std::filesystem::path GetConfigPath();

        static void DrawProjectList();
        static void DrawNewProjectDialog();

        static inline std::vector<RecentProject> s_RecentProjects;
        static inline bool s_Visible = false;
        static inline bool s_ShowNewProjectDialog = false;
        static inline std::filesystem::path s_PendingProject;
        static inline bool s_HasPending = false;
    };
}
