#pragma once

#include "luth/core/types/LuthMath.h"
#include "luth/scene/Entity.h"

#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace Luth
{
    class Window;
    class Scene;

    // Editor-owned simulation state. The engine queries this via IEditorHooks to decide whether
    // game systems (Animation, future Physics, Scripting) should tick on the current frame.
    // A headless runtime leaves the hook registry empty, in which case the engine defaults to
    // "always tick".
    enum class PlayState
    {
        Editing,
        Playing,
        Paused,
    };

    // Physics debug-visualization colour scheme. Used by PhysicsSystem's debug-draw passes; the
    // editor selects the mode via Preferences and forwards it through EditorViewportState. Uniform
    // applies physicsUniformColor to every body. ByMotionType uses fixed greys/blues/greens.
    // BySleepState matches Jolt's SleepColor (static grey, kinematic green, dynamic-active yellow,
    // sleeping red) and queries JPH::BodyInterface::IsActive per body.
    enum class PhysicsDebugColorMode : u8 { Uniform, ByMotionType, BySleepState };

    // Per-frame snapshot of editor-owned state that the engine feeds into RenderingSystem. When
    // no editor is registered the struct stays default-constructed, so the runtime build sees an
    // identity camera and empty selection.
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
        bool      enableVolumetricFog = true;
        std::vector<Entity> selectedEntities;

        // Selection-outline + editor-grid params.
        // Defaults keep a runtime build (no editor) rendering sanely without an EditorViewportState producer.
        Vec4 outlineColor          = { 1.0f, 0.6f, 0.0f, 1.0f };
        float outlineWidth          = 1.5f;
        float outlineOccludedAlpha  = 0.65f;

        Vec4 gridAxisXColor    = { 0.80f, 0.10f, 0.15f, 1.00f };
        Vec4 gridAxisZColor    = { 0.10f, 0.25f, 0.80f, 1.00f };
        Vec4 gridColor         = { 0.41f, 0.41f, 0.41f, 0.50f };
        float gridMajorScale    = 1.0f;
        float gridFadeStart     = 20.0f;
        float gridFadeEnd       = 200.0f;
        float gridLineThickness = 1.0f;

        // When true, AnimationSystem still ticks while PlayState::Editing so
        // characters animate in the scene view. Flip off for strict
        // "game systems only run during Play" behavior.
        bool      previewAnimationInEditor = true;

        // Physics debug visualization. Paired toggles per pass: render bodies of selected entities,
        // and render bodies of all entities (overrides selected when on). Colour scheme + segment
        // count + uniform colour shape what each pass looks like; alpha-unselected dims non-selected
        // bodies when "All" is on so the selected one still pops.
        bool                  physicsShapesSelected  = true;
        bool                  physicsShapesAll       = false;
        bool                  physicsAABBsSelected   = false;
        bool                  physicsAABBsAll        = false;
        bool                  physicsCoMSelected     = false;
        bool                  physicsCoMAll          = false;
        PhysicsDebugColorMode physicsColorMode       = PhysicsDebugColorMode::Uniform;
        Vec4                  physicsUniformColor    = { 0.40f, 0.86f, 0.37f, 1.0f };
        u32                   physicsDebugSegments   = 32;
        float                 physicsAlphaUnselected = 0.6f;
    };

    // Interface the engine uses to drive the editor without ever depending on luthien/ headers.
    // Luthien.lib provides the concrete implementation and registers it via EditorHooks::Register
    // before App is constructed. A headless or runtime-only build leaves the registry empty and
    // the engine quietly skips editor-specific behavior. See arch/editor.md for the contract.
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

        // Play-mode state + single-frame step request. Defaults keep the
        // runtime-only (no-editor) build "always ticking" game systems.
        virtual PlayState GetPlayState() const { return PlayState::Editing; }
        virtual bool ConsumeStepRequest() { return false; }

        // ProjectPanel: current directory for file-drop ingestion.
        // Returns empty path if no panel is available.
        virtual std::filesystem::path GetProjectCurrentDir() = 0;

        // Engine-side notice surfaced to the editor UI (e.g. status bar, log).
        // Default no-op so runtime-only builds (no editor) can ignore it.
        // Currently used by FrameDebuggerContext when a captured view is closed
        // mid-Freeze and capture is auto-cleared.
        virtual void OnFrameDebuggerNotice(const std::string& /*message*/) {}

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
        // Registered by Luthien.lib's bootstrap before App is constructed. Passing nullptr clears
        // the registration (rarely useful in practice — exists for symmetry).
        void Register(IEditorHooks* hooks);

        // Returns the registered hook, or nullptr if no editor is linked (runtime-only build) or
        // if hooks haven't been registered yet. All call sites must nullptr-check.
        IEditorHooks* Get();
    }
}
