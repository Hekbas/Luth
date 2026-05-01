#pragma once

// Per-frame editor snapshot — the frame boundary between gather (worker fibers) and
// draw (main thread). Mirrors the engine's RenderSnapshot pattern at smaller scale:
// gather workers fill per-panel fragments into each panel's own LinearAllocator,
// then Editor::Render assembles the read-only EditorSnapshot view on main and hands
// it to OnDraw. ImGui calls happen exclusively in OnDraw against the frozen view.
//
// Storage strategy is panel-local rather than a shared TaggedPageAllocator tag —
// the editor is single-frame (gather + draw run inside one App::Run iteration on
// the same thread sequence), not pipelined like Game/Render. TaggedPageAllocator's
// FreeTag would leak pages held in JobContext.CpuCache for fibers that don't run
// gather every frame. See plan §C and history/v2.x/editor-foundation.md for the
// full rationale.

#include "luthien/Editor.h"   // Panel definition (m_GatherAlloc, m_SnapshotFragment, m_FragmentType)

#include <typeindex>
#include <unordered_map>
#include <utility>

namespace Luth
{
    // Read-only view assembled by Editor on main thread after gather workers finish.
    // Type-keyed; OnDraw asks Get<T>() and gets the panel's fragment (or nullptr if
    // the panel didn't run gather this frame).
    class EditorSnapshot
    {
    public:
        template<typename T>
        const T* Get() const
        {
            auto it = m_Fragments.find(std::type_index(typeid(T)));
            return it != m_Fragments.end() ? static_cast<const T*>(it->second) : nullptr;
        }

    private:
        friend class Editor;
        std::unordered_map<std::type_index, const void*> m_Fragments;
    };

    // Passed to OnGather on a worker fiber. Allocates the panel's snapshot fragment
    // into its own m_GatherAlloc (panel-local — no cross-fiber sync) and records the
    // typed pointer back on the panel for later snapshot assembly on main.
    //
    // V3 contract: must not touch ImGui or VkCommandBuffer from OnGather. Builder
    // only writes to the panel's own allocator + fields — safe per V1 (no shared
    // mutable state, no locks needed).
    class EditorSnapshotBuilder
    {
    public:
        explicit EditorSnapshotBuilder(Panel& owner) : m_Owner(owner) {}

        // Allocate T into the owner panel's gather scratch and register it as the
        // panel's snapshot fragment. Subsequent Add<U> calls overwrite — one fragment
        // per panel by design.
        template<typename T, typename... Args>
        T* Add(Args&&... args)
        {
            T* p = m_Owner.m_GatherAlloc.New<T>(std::forward<Args>(args)...);
            m_Owner.m_SnapshotFragment = p;
            m_Owner.m_FragmentType = typeid(T);
            return p;
        }

        // Direct allocator handle for span/string allocation inside the fragment.
        Memory::LinearAllocator& Alloc() { return m_Owner.m_GatherAlloc; }

    private:
        Panel& m_Owner;
    };
}
