# Phase 5: Rendering Foundation & Features — Debugging ✅ (2026-03-15)

After Phases 1–4, models dragged into the hierarchy were invisible. Five root causes were diagnosed and fixed.

### Root Causes & Fixes

#### Fix 1 — SceneColor Texture Disconnect
**File:** `RenderingSystem.cpp` — `AddGeometryPass()`

The RenderGraph geometry pass was calling `builder.CreateTexture(desc)` which allocates a **transient** (pooled, temporary) texture from `RenderResourceCache`. Geometry was rendered to that throwaway texture. `ScenePanel` was displaying `m_SceneColor` — a completely separate `VKTexture` owned by `RenderingSystem`. The two never met.

**Fix:** Import `m_SceneColor` as an external resource:
```cpp
auto vkTex = std::static_pointer_cast<VKTexture>(m_SceneColor);
data.outputTex = rg.ImportResource(desc,
    (void*)vkTex->GetImage(),
    (void*)vkTex->GetImageView(),
    RG::ResourceState::ShaderResource);
```
External resources (`external=true`, `isTransient=false`) are not cleaned up by `CleanupPhysicalResources()`.

---

#### Fix 2 — Depth Clear = 0.0 (All Fragments Failed)
**File:** `RenderingSystem.cpp` — `AddGeometryPass()` setup lambda

`builder.WriteDepth(data.depthTex, LOAD_OP_CLEAR, ..., {})` — `VkClearValue{}` zero-initializes, so `depthStencil.depth = 0.0f`. With `VK_COMPARE_OP_LESS`, every fragment needs depth < 0.0 to pass — impossible.

**Fix:**
```cpp
VkClearValue depthClear{};
depthClear.depthStencil = { 1.0f, 0 };
data.depthTex = builder.WriteDepth(data.depthTex,
    VK_ATTACHMENT_LOAD_OP_CLEAR, VK_ATTACHMENT_STORE_OP_DONT_CARE, depthClear);
```

---

#### Fix 3 — Bindless Index 0 Unbound
**File:** `VulkanDescriptors.cpp` — `Init()`

`CreateNullTexture()` allocated a 1×1 white image+view but never wrote it into any descriptor slot. Index 0 was popped from `m_FreeIndices` by the first real texture load, leaving no null fallback at slot 0. Default `albedoMapIndex = 0` in push constants sampled an unbound descriptor → undefined behavior.

**Fix:** After `CreateNullTexture()`, manually reserve index 0:
```cpp
u32 reservedIndex = m_FreeIndices.front();
m_FreeIndices.pop_front(); // = 0
// Write null texture view into descriptor slot 0
vkUpdateDescriptorSets(...);
```

---

#### Fix 4 — Y-Flip Baked into EditorCamera (ImGuizmo Mismatch)
**Files:** `ScenePanel.cpp` (EditorCamera), `RenderingSystem.cpp`

`EditorCamera::UpdateProjection()` was applying `m_ProjectionMatrix[1][1] *= -1.0f` — a Vulkan Y-flip baked into the matrix returned by `GetProjectionMatrix()`. ImGuizmo calls `GetProjectionMatrix()` to place gizmos in screen space. It expects OpenGL-convention NDC (Y-up). The Y-flip inverted all of ImGuizmo's internal screen-space calculations → gizmo and model moved in opposite directions on pan.

**Fix:**
- Remove Y-flip from `EditorCamera::UpdateProjection()` entirely.
- Apply only during GPU uniform upload in `RenderingSystem::UpdateGlobalUniforms()`:
```cpp
ubo.projection = camera.GetProjectionMatrix();
ubo.projection[1][1] *= -1.0f;  // Vulkan Y-flip (shader only, not ImGuizmo)
ubo.viewProjection = ubo.projection * ubo.view;
```

---

#### Fix 5 — Front Face Winding (Models Inside-Out)
**File:** `RenderingSystem.cpp` — constructor, pipeline config

Pipeline had `VK_FRONT_FACE_CLOCKWISE`, reasoning that the projection Y-flip reverses CCW→CW winding. This forgot that `glm::lookAt` also reverses winding — its view matrix rotation has **det = −1** (negates the forward axis, implicit reflection). Two reversals cancel:

| Stage | Winding | Why |
|---|---|---|
| Model space | CCW | Standard vertex ordering |
| After view matrix (det=−1) | CW | `glm::lookAt` reflection |
| After projection Y-flip (det=−1) | CCW | Second reversal cancels first |
| **Clip space** | **CCW** | **Net result** |

`FRONT_FACE_CLOCKWISE` was treating CCW as "back face" → culling all visible surfaces → you could only see the inside of the model.

**Fix:**
```cpp
config.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
```

### Files Modified

| File | Changes |
|---|---|
| `luth/scene/systems/RenderingSystem.cpp` | Fix 1 (import resource), Fix 2 (depth clear), Fix 4 (Y-flip in UBO only), Fix 5 (CCW winding) |
| `luth/editor/panels/ScenePanel.cpp` | Fix 4 (remove Y-flip from EditorCamera) |
| `luth/renderer/backend/vulkan/VulkanDescriptors.cpp` | Fix 3 (reserve bindless slot 0 for null texture) |

### Commit
```
fix(renderer): fix 5 rendering bugs — models now visible and correctly oriented

- Import m_SceneColor as external RenderGraph resource (was transient/discarded)
- Fix depth clear value: 1.0f (was 0.0f, failed all depth tests)
- Reserve bindless descriptor slot 0 for null texture fallback
- Move Vulkan Y-flip to GPU uniform upload only (fixes ImGuizmo alignment)
- Fix front face winding: COUNTER_CLOCKWISE (lookAt det=-1 + Y-flip det=-1 cancel)
```
