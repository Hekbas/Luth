#pragma once

#include "luth/core/types/LuthTypes.h"

namespace Luth
{
    // Periodic side-channel scene backup. Autosaves go to
    // <project>/.luth/autosaves/<stem>-<TS>.luth — never the canonical scene.
    // Dirty flag is read-only here; manual Save remains the only path that
    // clears it. Gated on PlayState::Editing via PlayStateChangedSignal.
    class EditorAutoSave
    {
    public:
        static void Init();
        static void Shutdown();

        // Called once per frame from Editor::Render. Cheap when nothing to do.
        static void Tick();

        // File > Autosave Now hook. Bypasses the interval gate but honours all
        // other guards (Play mode, Untitled, foreign-path, !Dirty).
        static void ForceNow();

        // Title-bar UX — UpdateWindowTitle queries these to append a fading
        // " — Autosaved HH:MM" suffix.
        static const char* GetLastNotice();   // empty string when unexpired-or-absent
        static bool        IsNoticeActive();
    };
}
