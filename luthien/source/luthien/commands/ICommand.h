#pragma once

#include <memory>
#include <vector>

namespace Luth
{
    // Editor command base. Every user mutation that should be undoable subclasses this and is
    // pushed onto CommandHistory. CanMerge / MergeWith let continuous edits (gizmo drags,
    // slider scrubs) coalesce into a single history entry instead of producing dozens.
    // CompoundCommand bundles multiple commands so one undo backs out a logical group.
    class ICommand
    {
    public:
        virtual ~ICommand() = default;
        virtual void Execute() = 0;
        virtual void Undo() = 0;
        virtual void Redo() { Execute(); }
        virtual const char* GetName() const = 0;

        // Coalescing for continuous edits (gizmo drags, sliders).
        virtual bool CanMerge(const ICommand&) const { return false; }
        virtual void MergeWith(const ICommand&) {}
    };

    class CompoundCommand : public ICommand
    {
    public:
        CompoundCommand(const char* name, std::vector<std::unique_ptr<ICommand>> cmds)
            : m_Name(name), m_Commands(std::move(cmds)) {}

        void Execute() override {
            for (auto& cmd : m_Commands) cmd->Execute();
        }
        void Undo() override {
            for (auto it = m_Commands.rbegin(); it != m_Commands.rend(); ++it)
                (*it)->Undo();
        }
        void Redo() override {
            for (auto& cmd : m_Commands) cmd->Redo();
        }
        const char* GetName() const override { return m_Name; }
        bool IsEmpty() const { return m_Commands.empty(); }
        size_t GetChildCount() const { return m_Commands.size(); }
        const std::vector<std::unique_ptr<ICommand>>& GetChildren() const { return m_Commands; }

    private:
        const char* m_Name;
        std::vector<std::unique_ptr<ICommand>> m_Commands;
    };
}
