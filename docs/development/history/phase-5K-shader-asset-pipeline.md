# Phase 5-K: Shader Asset Pipeline + Material Pipeline Fix ✅ (2026-03-19)

**Goal:** Treat shaders as proper importable assets with SPIR-V artifact caching and stable UUIDs. Fix two critical bugs in `AssetDatabase::Init` and a `MaterialSystem` dirty-flag sync issue.

### Shader Asset Pipeline

`ShaderImporter` compiles `.vert`+`.frag` pairs to SPIR-V via shaderc and serializes to a binary artifact format `[AssetHeader][ShaderHeader][VertSpirV][FragSpirV]`. `Shader::Create(vertSpv, fragSpv, path)` factory overload loads from pre-compiled SPIR-V without recompilation. `RenderingSystem` loads the PBR shader via `AssetDatabase::GetUUID` + `AssetManager::LoadImmediate` for UUID-stable references across restarts.

### Bug Fixes

1. **AssetDatabase iterator UB** — `Init()` was modifying the directory (creating `.meta` files) during `recursive_directory_iterator` traversal. Fixed by collecting all paths first, then creating metas.
2. **UUID default ctor always valid** — `UUID uuid = UUID()` generates a valid UUID, so `!uuid.IsValid()` was always false — `MetaFile::Create` was never called for new assets. Fixed to `UUID::Invalid()`.
3. **MaterialSystem dirty sync** — now checks both `MaterialSlot::dirty` and `Material::IsDirty()` each frame.

### Files

| File | Changes |
|---|---|
| `luth/resources/importers/ShaderImporter.h/.cpp` | NEW — SPIR-V compilation + artifact serialization |
| `luth/resources/AssetSerializer.h/.cpp` | `SerializeShader`/`DeserializeShader` binary format |
| `luth/resources/AssetManager.cpp` | Registered `ShaderImporter`; handle `AssetType::Shader` in load paths |
| `luth/renderer/Shader.h/.cpp` | `Create(vertSpv, fragSpv, path)` factory overload |
| `luth/renderer/backend/vulkan/VulkanShader.cpp` | Constructor from pre-compiled SPIR-V |
| `luth/resources/FileSystem.cpp` | `.vert` → `AssetType::Shader` mapping |
| `luth/scene/systems/RenderingSystem.cpp` | Load PBR shader via UUID |
| `luth/resources/importers/ModelImporter.cpp` | Assign PBR UUID at material creation time |
| `luth/editor/panels/InspectorPanel.cpp` | Shader combo always visible; Save writes source + artifact |
| `luth/renderer/MaterialSystem.cpp` | Dual dirty-flag check |
| `luth/resources/AssetDatabase.cpp` | Collect-then-create meta fix; `UUID::Invalid()` fix |
