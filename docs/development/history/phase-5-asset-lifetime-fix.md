# Asset Lifetime + Loading Pipeline Fix ✅ (2026-03-21)

**Goal:** Prevent textures/materials from being garbage-collected while still in use by scene entities. Fix full asset loading chain so models dropped into the scene immediately display with textures. Fix VMA shutdown crash caused by GPU allocations outliving the allocator.

### Root Cause Analysis

`AssetManager::Trim()` evicts assets with `shared_ptr::use_count() == 1` after 5 seconds idle. Since `MeshRenderer` components only store UUIDs (no `shared_ptr`), and the render loop creates temporary `shared_ptr`s that die at end of scope, the cache was always the sole holder — assets were evicted while visually in use.

Two additional bugs: `MaterialSystem` stored raw `Material*` pointers (dangling on eviction), and `App::m_Scene` was destroyed after `Renderer::Shutdown()`, causing VMA to assert on remaining allocations.

### Fix 1 — Scene asset hold system

`Scene` gained a `std::unordered_map<UUID, std::shared_ptr<Asset>, UUIDHash> m_HeldAssets` and three methods: `HoldAsset()`, `ReleaseAsset()`, `ReleaseAllAssets()`. `HoldAsset` also transitively holds texture dependencies when the asset is a Material. `Clear()` calls `ReleaseAllAssets()` first. With held references, `use_count >= 2` — `Trim()` naturally skips them.

### Fix 2 — MaterialSystem shared_ptr

`MaterialSlot::material` changed from raw `Material*` to `std::shared_ptr<Material>`. `RegisterMaterial` signature updated to accept `shared_ptr<Material>`. This eliminates the dangling pointer bug and keeps materials alive while registered in the SSBO. `EnsureMaterialRegistered` in `RenderingSystem` updated to match.

### Fix 3 — RenderingSystem holds assets each frame

In `RenderingSystem::Update()`, the material registration loop now calls `scene->HoldAsset()` for models and materials (which transitively holds textures) before registering the material.

### Fix 4 — Material loading triggers texture loading

In `HierarchyPanel::InstantiateModel()`, after assigning `mr.MaterialUUID`, `AssetManager::LoadAsync(materialUUID)` is now called. In `AssetManager::Update()`, when a material finishes loading, `LoadAsync` is called for each texture UUID in its map list — ensuring the full dependency chain loads without manual intervention.

### Fix 5 — MaterialSystem detects stale bindless indices

`MaterialSystem::Update()` now always calls `UpdateGPUData()` for every registered material and compares the result to the previous GPU data via `memcmp`. If any bindless indices changed (e.g. a texture finished async loading), the SSBO slot is re-uploaded — no dirty flag required.

### Fix 6 — Clean shutdown order

`MaterialSystem::Shutdown()` clears `m_Slots` and `m_FreeIndices` before destroying Vulkan resources, releasing all `shared_ptr<Material>` references. `App::Close()` calls `m_Scene->Clear()` after `AssetManager::Shutdown()` and before `Renderer::FlushDeletionQueues()`, ensuring all GPU resources are released and queued for deletion before `vmaDestroyAllocator` is called.

### Files Modified

| File | Changes |
|---|---|
| `luth/scene/Scene.h` | `m_HeldAssets` map, `HoldAsset`/`ReleaseAsset`/`ReleaseAllAssets` API |
| `luth/scene/Scene.cpp` | Implement Hold/Release; `ReleaseAllAssets()` in `Clear()`; includes for Material, Texture, AssetManager |
| `luth/renderer/MaterialSystem.h` | `MaterialSlot::material` → `shared_ptr<Material>`; `RegisterMaterial` takes `shared_ptr` |
| `luth/renderer/MaterialSystem.cpp` | Updated `RegisterMaterial` param; `Update()` uses `memcmp` to detect bindless index changes; `Shutdown()` clears slots |
| `luth/scene/systems/RenderingSystem.h` | `EnsureMaterialRegistered` takes `shared_ptr<Material>` |
| `luth/scene/systems/RenderingSystem.cpp` | Material loop holds assets in scene; passes `shared_ptr` to `EnsureMaterialRegistered`; includes `Scene.h` |
| `luth/editor/panels/HierarchyPanel.cpp` | `InstantiateModel()` calls `LoadAsync` for material UUIDs |
| `luth/resources/AssetManager.cpp` | `Update()` triggers `LoadAsync` for texture deps when material finishes loading |
| `luth/core/App.cpp` | `Close()` calls `m_Scene->Clear()` before `Renderer::FlushDeletionQueues()` |
