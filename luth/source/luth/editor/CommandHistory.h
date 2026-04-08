#pragma once

#include "luth/editor/Command.h"
#include "luth/core/LuthTypes.h"

#include <memory>
#include <vector>
#include <string>

namespace Luth
{

    class CommandHistory
    {
    public:
        // Push and execute a command
        static void Execute(std::unique_ptr<ICommand> cmd);

        // Undo / Redo
        static void Undo();
        static void Redo();

        // Compound command grouping
        static void BeginCompound(const char* name);
        static void EndCompound();

        // Query
        static bool CanUndo();
        static bool CanRedo();
        static const char* GetUndoName();
        static const char* GetRedoName();

        // Clear history (on scene change)
        static void Clear();

        // Read-only stack access (for debug panel)
        static const std::vector<std::unique_ptr<ICommand>>& GetUndoStack() { return s_UndoStack; }
        static const std::vector<std::unique_ptr<ICommand>>& GetRedoStack() { return s_RedoStack; }
        static bool IsInCompound() { return s_InCompound; }
        static const char* GetCompoundName() { return s_CompoundName; }

    private:
        static inline std::vector<std::unique_ptr<ICommand>> s_UndoStack;
        static inline std::vector<std::unique_ptr<ICommand>> s_RedoStack;
        static inline std::vector<std::unique_ptr<ICommand>> s_CompoundBuffer;
        static inline const char* s_CompoundName = nullptr;
        static inline bool s_InCompound = false;
        static constexpr u32 kMaxHistorySize = 100;
    };
}
