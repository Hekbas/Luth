# Phase 5-C: Shader Hot-Reload + ShaderLibrary ✅ (2026-03-15)

ShaderLibrary singleton for shader management. File watcher triggers pipeline rebuild on `.vert`/`.frag` changes. Inspector shader combo via `ShaderLibrary::GetAll()`.

SPIRV-Cross reflection deferred: Vulkan SDK pre-built libs crash at runtime (STL ABI mismatch). `Reflect()` body preserved, guarded by `LUTH_SPIRV_CROSS_ENABLED`.

### Files Created/Modified

| File | Changes |
|---|---|
| `luth/renderer/Shader.h/.cpp` | `Reload()`, `IsValid()`, `GetSpirV()`, `GetShaderModule()` |
| `luth/renderer/ShaderLibrary.h/.cpp` | Singleton — `Register()`, `GetAll()`, `GetByName()`, `GetByUUID()` |
| `luth/editor/panels/InspectorPanel.cpp` | Shader combo using `ShaderLibrary::GetAll()` |
