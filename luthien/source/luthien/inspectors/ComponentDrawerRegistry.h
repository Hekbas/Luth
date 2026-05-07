#pragma once

#include "luth/scene/Entity.h"
#include "luthien/CommandHistory.h"
#include "luthien/EditorClipboard.h"
#include "luthien/commands/ComponentCommands.h"
#include "luthien/widgets/CollapsingHeader.h"

#include <functional>
#include <string>
#include <vector>
#include <typeindex>
#include <memory>
#include <utility>
#include <imgui.h>

namespace Luth
{
    // Global registry of per-component-type ImGui drawers for the InspectorPanel.
    // Register<T>(name, drawFn) at editor init wires up the inspector row plus its context-menu
    // actions (copy / paste / reset / remove). Each context-menu action is backed by an ICommand
    // wrapper, so every component edit routes through CommandHistory and is undoable.
    struct ComponentDrawerOptions
    {
        bool Removable     = true;
        bool ShowInAddMenu = true;
        std::string AddLabel;                                       // empty → Name
        std::function<bool(Entity)> CanAdd;                         // empty → !HasComponent<T>()
        std::function<void(Entity)> OnAdd;                          // empty → issue ComponentAddCommand<T>
        std::function<void(Entity)> OnReset;                        // empty → issue ComponentResetCommand<T>
        std::function<std::string(Entity)> OnCopy;                  // empty → menu item disabled
        std::function<bool(Entity, const std::string&)> OnPaste;    // empty → menu item disabled
    };

    struct ComponentDrawerDescriptor
    {
        std::string                 Name;
        std::string                 AddCommandName;    // "Add <Name>" — owns the const char* lifetime
        std::type_index             Type;
        entt::id_type               ComponentTypeId;   // entt::type_hash<T>::value() — clipboard key
        std::function<bool(Entity)> HasComponent;
        std::function<void(Entity)> Draw;
        std::function<bool(Entity)> CanAdd;
        std::function<void(Entity)> OnAdd;
        std::function<void(Entity)> OnReset;
        std::function<std::string(Entity)> OnCopy;
        std::function<bool(Entity, const std::string&)> OnPaste;
        bool Removable     = true;
        bool ShowInAddMenu = true;
    };

    class ComponentDrawerRegistry
    {
    public:
        template<typename T, typename DrawFn>
        static void Register(const char* name, DrawFn drawFn, ComponentDrawerOptions opts = {});

        template<typename T, typename DrawFn>
        static void RegisterSimple(const char* name, DrawFn drawFn) {
            Register<T>(name, std::move(drawFn));
        }

        static const std::vector<ComponentDrawerDescriptor>& GetDrawers() { return s_Drawers; }
        static void Shutdown() { s_Drawers.clear(); }

    private:
        static std::vector<ComponentDrawerDescriptor> s_Drawers;
    };

    template<typename T, typename DrawFn>
    void ComponentDrawerRegistry::Register(const char* name, DrawFn drawFn, ComponentDrawerOptions opts)
    {
        // Reserve once — descriptors hold std::functions whose captured strings
        // must remain at a stable address after the push_back move settles.
        if (s_Drawers.capacity() == 0) s_Drawers.reserve(32);

        ComponentDrawerDescriptor d{
            /*Name*/            name,
            /*AddCommandName*/  std::string("Add ") + (opts.AddLabel.empty() ? std::string(name) : opts.AddLabel),
            /*Type*/            std::type_index(typeid(T)),
            /*ComponentTypeId*/ entt::type_hash<T>::value(),
            /*HasComponent*/    [](Entity e) { return e.HasComponent<T>(); },
            /*Draw*/            {},
            /*CanAdd*/          {},
            /*OnAdd*/           {},
            /*OnReset*/         {},
            /*OnCopy*/          std::move(opts.OnCopy),
            /*OnPaste*/         std::move(opts.OnPaste),
            /*Removable*/       opts.Removable,
            /*ShowInAddMenu*/   opts.ShowInAddMenu,
        };

        d.CanAdd = opts.CanAdd
            ? std::move(opts.CanAdd)
            : std::function<bool(Entity)>{[](Entity e) { return !e.HasComponent<T>(); }};

        if (opts.OnAdd) {
            d.OnAdd = std::move(opts.OnAdd);
        } else {
            std::string addCmdName = d.AddCommandName;
            d.OnAdd = [addCmdName = std::move(addCmdName)](Entity e) {
                CommandHistory::Execute(std::make_unique<ComponentAddCommand<T>>(
                    addCmdName.c_str(), e.GetScene(), (entt::entity)e));
            };
        }

        if (opts.OnReset) {
            d.OnReset = std::move(opts.OnReset);
        } else {
            std::string resetCmdName = std::string("Reset ") + name;
            d.OnReset = [resetCmdName = std::move(resetCmdName)](Entity e) {
                CommandHistory::Execute(std::make_unique<ComponentResetCommand<T>>(
                    resetCmdName.c_str(), e.GetScene(), (entt::entity)e));
            };
        }

        // Draw built last so the contextMenu lambda can capture OnReset/OnCopy/
        // OnPaste by value — std::function copies are independent of d's move
        // into s_Drawers below.
        std::string headerName = name;
        bool removable = opts.Removable;
        auto userDraw = std::move(drawFn);
        auto onReset  = d.OnReset;
        auto onCopy   = d.OnCopy;
        auto onPaste  = d.OnPaste;
        const entt::id_type typeId = d.ComponentTypeId;

        d.Draw = [
            headerName = std::move(headerName),
            removable, userDraw = std::move(userDraw),
            onReset = std::move(onReset),
            onCopy  = std::move(onCopy),
            onPaste = std::move(onPaste),
            typeId
        ](Entity entity) mutable {
            if (!entity.HasComponent<T>()) return;
            auto contextMenu = [entity, removable, &onReset, &onCopy, &onPaste, typeId]() {
                if (ImGui::MenuItem("Reset Values"))
                    onReset(entity);

                bool canCopy = static_cast<bool>(onCopy);
                if (ImGui::MenuItem("Copy Component", nullptr, nullptr, canCopy)) {
                    EditorClipboard::SetComponent(typeId, onCopy(entity));
                }

                bool canPaste = static_cast<bool>(onPaste) && EditorClipboard::HasComponent(typeId);
                if (ImGui::MenuItem("Paste Component Values", nullptr, nullptr, canPaste)) {
                    if (auto data = EditorClipboard::GetComponent(typeId))
                        onPaste(entity, *data);
                }

                ImGui::Separator();

                if (ImGui::MenuItem("Remove Component", nullptr, nullptr, removable)) {
                    CommandHistory::Execute(std::make_unique<ComponentRemoveCommand<T>>(
                        "Remove Component", entity.GetScene(), (entt::entity)entity));
                }
            };
            if (UI::BeginCollapsingHeader(headerName.c_str(), true, contextMenu)) {
                // contextMenu runs INSIDE BeginCollapsingHeader; Remove Component
                // synchronously detaches T before this line runs. Re-guard.
                if (entity.HasComponent<T>())
                    userDraw(entity, entity.GetComponent<T>());
                UI::EndCollapsingHeader();
            }
        };

        s_Drawers.push_back(std::move(d));
    }
}
