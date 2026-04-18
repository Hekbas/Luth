#pragma once

#include "luth/core/LuthTypes.h"
#include "luth/scene/Entity.h"

#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace Luth
{
    class Window;
    class Scene;

    /// Per-frame snapshot of editor-owned state the engine feeds into
    /// RenderingSystem. When no editor is registered, stays default-constructed
    /// and the engine uses identity camera + empty selection.
    struct EditorViewportState
    {
        bool      hasCamera        = false;
        Mat4 view             = Mat4(1.0f);
        Mat4 projection       = Mat4(1.0f);
        Vec3 position         = Vec3(0.0f);
        float     nearZ            = 0.1f;
        float     farZ             = 1000.0f;
        float     iblIntensity     = 1.0f;
        float     skyboxIntensity  = 1.0f;
        std::vector<Entity> selectedEntities;
    };

    /// Interface the engine uses to drive the editor without depending on
    /// luthien/ headers. The editor library (Luthien.lib) provides a concrete
    /// implementation and registers it via EditorHooks::Register before App
    /// construction. A headless/runtime-only build leaves the registry empty
    /// and the engine skips editor-specific behavior.
    struct IEditorHooks
    {
        virtual ~IEditorHooks() = default;

        // Lifecycle
        virtual void Init(Window* window) = 0;
        virtual void SetActiveScene(std::shared_ptr<Scene> scene) = 0;
        virtual void Shutdown() = 0;

        // Per-frame
        virtual void BeginFrame() = 0;
        virtual void Render() = 0;
        virtual void EndFrame() = 0;

        // Project lifecycle
        virtual void OnProjectChanged() = 0;
        virtual void SaveSettings() = 0;

        // Input capture queries (used by Input.cpp to suppress keys/mouse
        // that ImGui is consuming)
        virtual bool WantCaptureKeyboard() = 0;
        virtual bool WantCaptureMouse() = 0;

        // Editor-owned viewport / selection snapshot fed to RenderingSystem
        virtual void GetViewportState(EditorViewportState& out) = 0;

        // ProjectPanel: current directory for file-drop ingestion.
        // Returns empty path if no panel is available.
        virtual std::filesystem::path GetProjectCurrentDir() = 0;

        // Project launcher
        virtual void ShowProjectLauncher() = 0;
        virtual bool HasPendingProject() = 0;
        virtual std::filesystem::path ConsumePendingProject() = 0;
        virtual void AddRecentProject(const std::string& name, const std::filesystem::path& path) = 0;
        virtual void HideProjectLauncher() = 0;
        virtual void SetPendingProject(const std::filesystem::path& path) = 0;
    };

    namespace EditorHooks
    {
        /// Registered by Luthien.lib's bootstrap before App is constructed.
        /// Passing nullptr clears the registration (rarely useful).
        void Register(IEditorHooks* hooks);

        /// Returns the registered hook, or nullptr if no editor is linked
        /// (runtime-only build) or hooks haven't been registered yet.
        /// Call sites must nullptr-check.
        IEditorHooks* Get();
    }
}
