#pragma once

#include "luthien/commands/Commands.h"
#include "luth/core/types/LuthTypes.h"

#include <memory>
#include <deque>
#include <vector>
#include <string>

namespace Luth
{
    // Static undo/redo stack for the editor. Every user-visible mutation (entity create/destroy,
    // component add/remove, property change) flows through Execute(unique_ptr<ICommand>) and is
    // replayable via Undo/Redo. Compound mode (BeginCompound/EndCompound) groups multiple edits
    // under a single history entry, used for things like "paste prefab" where the user expects
    // one undo to back out the whole operation. PlayModeController calls SetBlocked(true) during
    // Play so gizmo and inspector edits don't dirty the snapshot-restored scene.

    class CommandHistory
    {
    public:
        static void Execute(std::unique_ptr<ICommand> cmd);

        static void Undo();
        static void Redo();

        static void BeginCompound(const char* name);
        static void EndCompound();

        static bool CanUndo();
        static bool CanRedo();
        static const char* GetUndoName();
        static const char* GetRedoName();

        // Clear history (on scene change)
        static void Clear();

        // Block mutations; PlayModeController flips this during Play/Stop. Execute / Undo / Redo /
        // BeginCompound / EndCompound all early-return when blocked, so gizmo + inspector edits during
        // play have no effect on the (snapshot-restored) scene.
        static void SetBlocked(bool blocked);
        static bool IsBlocked() { return s_Blocked; }

        // Read-only stack access (for debug panel)
        static const std::deque<std::unique_ptr<ICommand>>& GetUndoStack() { return s_UndoStack; }
        static const std::deque<std::unique_ptr<ICommand>>& GetRedoStack() { return s_RedoStack; }
        static bool IsInCompound() { return s_InCompound; }
        static const char* GetCompoundName() { return s_CompoundName; }

    private:
        // deque for O(1) pop_front on FIFO eviction when the stack exceeds kMaxHistorySize.
        static inline std::deque<std::unique_ptr<ICommand>> s_UndoStack;
        static inline std::deque<std::unique_ptr<ICommand>> s_RedoStack;
        static inline std::vector<std::unique_ptr<ICommand>> s_CompoundBuffer;
        static inline const char* s_CompoundName = nullptr;
        static inline bool s_InCompound = false;
        static inline bool s_Blocked = false;
        static inline bool s_WarnedBlocked = false;
        static constexpr u32 kMaxHistorySize = 100;
    };
}
