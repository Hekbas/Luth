#include "lepch.h"
#include "luthien/CommandHistory.h"
#include "luthien/commands/Commands.h"
#include "luthien/Editor.h"

namespace Luth
{
    void CommandHistory::Execute(std::unique_ptr<ICommand> cmd)
    {
        if (!cmd) return;

        cmd->Execute();

        if (s_InCompound)
        {
            s_CompoundBuffer.push_back(std::move(cmd));
            return;
        }

        // Check if we can merge with the top of the undo stack
        if (!s_UndoStack.empty() && s_UndoStack.back()->CanMerge(*cmd))
        {
            s_UndoStack.back()->MergeWith(*cmd);
        }
        else
        {
            s_UndoStack.push_back(std::move(cmd));
        }

        // New action invalidates the redo branch
        s_RedoStack.clear();

        // Cap history size
        while (s_UndoStack.size() > kMaxHistorySize)
            s_UndoStack.erase(s_UndoStack.begin());

        Editor::MarkDirty();
    }

    void CommandHistory::Undo()
    {
        if (s_UndoStack.empty()) return;

        auto cmd = std::move(s_UndoStack.back());
        s_UndoStack.pop_back();

        cmd->Undo();
        s_RedoStack.push_back(std::move(cmd));
        Editor::MarkDirty();
    }

    void CommandHistory::Redo()
    {
        if (s_RedoStack.empty()) return;

        auto cmd = std::move(s_RedoStack.back());
        s_RedoStack.pop_back();

        cmd->Redo();
        s_UndoStack.push_back(std::move(cmd));
        Editor::MarkDirty();
    }

    void CommandHistory::BeginCompound(const char* name)
    {
        LH_CORE_ASSERT(!s_InCompound, "Nested compound commands not supported");
        s_InCompound = true;
        s_CompoundName = name;
        s_CompoundBuffer.clear();
    }

    void CommandHistory::EndCompound()
    {
        LH_CORE_ASSERT(s_InCompound, "EndCompound without matching BeginCompound");
        s_InCompound = false;

        if (s_CompoundBuffer.empty()) return;

        auto compound = std::make_unique<CompoundCommand>(
            s_CompoundName, std::move(s_CompoundBuffer));

        s_UndoStack.push_back(std::move(compound));
        s_RedoStack.clear();

        while (s_UndoStack.size() > kMaxHistorySize)
            s_UndoStack.erase(s_UndoStack.begin());

        Editor::MarkDirty();
    }

    bool CommandHistory::CanUndo() { return !s_UndoStack.empty(); }
    bool CommandHistory::CanRedo() { return !s_RedoStack.empty(); }

    const char* CommandHistory::GetUndoName()
    {
        return s_UndoStack.empty() ? nullptr : s_UndoStack.back()->GetName();
    }

    const char* CommandHistory::GetRedoName()
    {
        return s_RedoStack.empty() ? nullptr : s_RedoStack.back()->GetName();
    }

    void CommandHistory::Clear()
    {
        s_UndoStack.clear();
        s_RedoStack.clear();
        s_CompoundBuffer.clear();
        s_InCompound = false;
        s_CompoundName = nullptr;
    }
}
