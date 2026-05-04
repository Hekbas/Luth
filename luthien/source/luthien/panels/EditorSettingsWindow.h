#pragma once

namespace Luth
{
    // Floating non-modal Preferences window. Opened via Edit > Preferences,
    // closed via title-bar X. Edits push to Editor::s_Settings on commit
    // (per-release EditState semantics) and trigger SaveSettings + ApplyPersistence.
    class EditorSettingsWindow
    {
    public:
        static void Show()  { s_Open = true; }
        static void Hide()  { s_Open = false; }
        static bool IsOpen() { return s_Open; }

        // Call once per frame from Editor::Render (after the panel loop).
        // No-op when closed.
        static void Draw();

    private:
        static inline bool s_Open = false;
    };
}
