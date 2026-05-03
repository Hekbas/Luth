#include "lepch.h"
#include "luthien/PlayModeController.h"
#include "luthien/Editor.h"
#include "luthien/CommandHistory.h"
#include "luthien/events/EditorSignals.h"
#include "luth/events/EventBus.h"
#include "luth/scene/Scene.h"
#include "luth/scene/SceneSerializer.h"

namespace Luth
{
    static void PublishPlayState(PlayState from, PlayState to)
    {
        EventBus::Enqueue<PlayStateChangedSignal>(BusType::MainThread, from, to);
    }

    void PlayModeController::EnterPlay()
    {
        if (s_State != PlayState::Editing) return;

        auto scene = Editor::GetActiveScene();
        if (!scene) {
            LH_CORE_WARN("PlayModeController::EnterPlay — no active scene");
            return;
        }

        s_Snapshot = SceneSerializer::SaveToString(*scene);
        s_SavedDirtyFlag = Editor::IsDirty();
        CommandHistory::Clear();
        CommandHistory::SetBlocked(true);

        const PlayState from = s_State;
        s_State = PlayState::Playing;
        s_StepRequested = false;
        PublishPlayState(from, s_State);
        LH_CORE_INFO("Play: enter");
    }

    void PlayModeController::Pause()
    {
        if (s_State != PlayState::Playing) return;
        const PlayState from = s_State;
        s_State = PlayState::Paused;
        PublishPlayState(from, s_State);
        LH_CORE_INFO("Play: pause");
    }

    void PlayModeController::Resume()
    {
        if (s_State != PlayState::Paused) return;
        const PlayState from = s_State;
        s_State = PlayState::Playing;
        PublishPlayState(from, s_State);
        LH_CORE_INFO("Play: resume");
    }

    void PlayModeController::Stop()
    {
        if (s_State == PlayState::Editing) return;

        auto scene = Editor::GetActiveScene();
        if (scene && !s_Snapshot.empty()) {
            if (!SceneSerializer::LoadFromString(*scene, s_Snapshot, /*preserveAssets=*/true)) {
                LH_CORE_ERROR("PlayModeController::Stop — failed to restore scene snapshot");
            }
        }
        s_Snapshot.clear();

        CommandHistory::SetBlocked(false);
        Editor::ResetDirtyState(s_SavedDirtyFlag);
        CommandHistory::Clear();

        const PlayState from = s_State;
        s_State = PlayState::Editing;
        s_StepRequested = false;
        PublishPlayState(from, s_State);
        LH_CORE_INFO("Play: stop (scene restored)");
    }

    void PlayModeController::RequestStep()
    {
        if (s_State != PlayState::Paused) return;
        s_StepRequested = true;
    }

    bool PlayModeController::ConsumeStepRequest()
    {
        const bool r = s_StepRequested;
        s_StepRequested = false;
        return r;
    }
}
