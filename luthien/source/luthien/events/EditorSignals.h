#pragma once

// EditorSignal — typed events on EventBus::BusType::MainThread that broadcast
// editor state mutations to subscribed panels. Replaces the v2.8.x hierarchy-
// version polling pattern.
//
// All signals are UUID-based (never raw entt::entity, per the v2.7.0 command
// precedent) so handles stay valid across destroy-undo cycles. Dispatched
// between frames by EventBus::ProcessEvents on the main thread; panels'
// OnEvent handlers can mutate panel state without racing OnGather.
//
// Reentrancy: a handler that enqueues another EditorSignal will see it fire
// on the NEXT ProcessEvents drain (next frame, typically). Don't rely on
// chained synchronous dispatch — write to panel state instead.

#include "luth/core/EditorHooks.h"   // PlayState
#include "luth/core/UUID.h"
#include "luth/events/Event.h"

#include <string>
#include <vector>

namespace Luth
{
    // Selection set or active resource changed. The version field mirrors
    // EditorSelection::GetVersion at publish time so subscribers can correlate
    // multiple in-flight events to a single ground-truth.
    class SelectionChangedSignal : public Event
    {
    public:
        SelectionChangedSignal(u32 version,
                               std::vector<UUID> entities,
                               UUID resource)
            : m_Version(version)
            , m_Entities(std::move(entities))
            , m_Resource(resource) {}

        u32 GetVersion() const { return m_Version; }
        const std::vector<UUID>& GetEntities() const { return m_Entities; }
        UUID GetResource() const { return m_Resource; }

        const char* GetName() const override { return "SelectionChangedSignal"; }
        u32 GetCategoryFlags() const override { return EventCategory::None; }

        static const char* GetStaticName() { return "SelectionChangedSignal"; }

    private:
        u32 m_Version;
        std::vector<UUID> m_Entities;
        UUID m_Resource;
    };

    // Scene hierarchy mutated by an EntityCommand. Subscribers that cache tree
    // topology (HierarchyPanel, FrameDebugger overlays) invalidate on receipt.
    class HierarchyChangedSignal : public Event
    {
    public:
        enum class Op : u8 { Created, Destroyed, Reparented, Reordered, Renamed };

        HierarchyChangedSignal(Op op, UUID entity, UUID parent = UUID::Invalid())
            : m_Op(op), m_Entity(entity), m_Parent(parent) {}

        Op    GetOp() const     { return m_Op; }
        UUID  GetEntity() const { return m_Entity; }
        UUID  GetParent() const { return m_Parent; }   // valid only for Op::Reparented

        const char* GetName() const override { return "HierarchyChangedSignal"; }
        u32 GetCategoryFlags() const override { return EventCategory::None; }

        static const char* GetStaticName() { return "HierarchyChangedSignal"; }

    private:
        Op   m_Op;
        UUID m_Entity;
        UUID m_Parent;
    };

    // Asset DB mutated (file-watch hot-reload, importer write, deletion).
    // ProjectPanel/ResourcePanel/InspectorPanel/ThumbnailCache subscribe.
    class AssetChangedSignal : public Event
    {
    public:
        enum class Op : u8 { Imported, Modified, Deleted };

        AssetChangedSignal(Op op, UUID asset)
            : m_Op(op), m_Asset(asset) {}

        Op   GetOp() const    { return m_Op; }
        UUID GetAsset() const { return m_Asset; }

        const char* GetName() const override { return "AssetChangedSignal"; }
        u32 GetCategoryFlags() const override { return EventCategory::None; }

        static const char* GetStaticName() { return "AssetChangedSignal"; }

    private:
        Op   m_Op;
        UUID m_Asset;
    };

    // Project switch (load / unload). Editor::OnProjectChanged publishes after
    // settings reload + scene clear so subscribers can rebuild project-scoped
    // caches in their handler.
    class ProjectChangedSignal : public Event
    {
    public:
        ProjectChangedSignal(std::string path, std::string name)
            : m_Path(std::move(path)), m_Name(std::move(name)) {}

        const std::string& GetPath() const        { return m_Path; }
        const std::string& GetProjectName() const { return m_Name; }   // GetName() reserved by Event base

        const char* GetName() const override { return "ProjectChangedSignal"; }
        u32 GetCategoryFlags() const override { return EventCategory::None; }

        static const char* GetStaticName() { return "ProjectChangedSignal"; }

    private:
        std::string m_Path;
        std::string m_Name;
    };

    // Play-mode state transition. AnimationSystem-gated panels, dirty-flag
    // controllers, autosave (v2.9.4+) all subscribe.
    class PlayStateChangedSignal : public Event
    {
    public:
        PlayStateChangedSignal(PlayState from, PlayState to)
            : m_From(from), m_To(to) {}

        PlayState GetFrom() const { return m_From; }
        PlayState GetTo() const   { return m_To; }

        const char* GetName() const override { return "PlayStateChangedSignal"; }
        u32 GetCategoryFlags() const override { return EventCategory::None; }

        static const char* GetStaticName() { return "PlayStateChangedSignal"; }

    private:
        PlayState m_From;
        PlayState m_To;
    };
}
