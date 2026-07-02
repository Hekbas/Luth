#pragma once

#include "luth/core/EditorHooks.h"

#include <string>

namespace Luth
{
    // Editor-owned play-mode state machine.
    //
    // On EnterPlay: the active scene is serialized to an in-memory JSON string
    // and CommandHistory is cleared. On Stop: the snapshot is restored via
    // SceneSerializer::LoadFromString(preserveAssets=true) so AssetManager
    // keeps existing assets; no mesh/texture re-resolve per Play->Stop cycle.
    // RequestStep/ConsumeStepRequest cooperate with App::Run so a single
    // engine frame can advance game systems while in the Paused state.
    class PlayModeController
    {
    public:
        static void EnterPlay();
        static void Pause();
        static void Resume();
        static void Stop();
        static void RequestStep();

        static PlayState GetState() { return s_State; }
        static bool ConsumeStepRequest();

    private:
        static inline PlayState s_State = PlayState::Editing;
        static inline std::string s_Snapshot;
        static inline bool s_StepRequested = false;
        static inline bool s_SavedDirtyFlag = false;
    };
}
