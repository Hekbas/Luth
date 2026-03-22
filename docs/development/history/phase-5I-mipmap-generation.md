# Phase 5-I: Mipmap Generation + Texture Import Settings ✅ (2026-03-19)

**Goal:** Full mip chain generation via `vkCmdBlitImage` at texture creation time, driven by per-texture `.meta` import settings. Also wired Inspector texture panel Apply button end-to-end.

### Architecture: TextureSettings Pipeline

Settings flow: `.meta type_settings` → `TextureImporter` → `TextureHeader` (artifact) → `TextureAssetData` → `Texture::Create(settings)` → `VKTexture`

```
.meta                    Artifact             GPU
──────────────────       ────────────         ──────────────────────────────
generate_mipmaps    →    TextureHeader   →    m_MipLevels, vkCmdBlitImage
wrap_mode           →    WrapMode        →    VkSamplerAddressMode
filter_min/mag      →    MinFilter       →    VkFilter
```

### Task 1 — TextureSettings struct + Create overload

Added `TextureSettings` to `Texture.h` carrying `GenerateMipmaps`, `WrapMode`, `MinFilter`, `MagFilter`. Added `TextureSettings Settings` field to `TextureAssetData`. Added `Texture::Create(..., const TextureSettings&)` factory overload.

### Task 2 — TextureHeader extended

`TextureHeader` in `AssetSerializer.h` gained 4 new `u32` fields. `SerializeTexture`/`DeserializeTexture` read/write them. Old artifacts deleted to force reimport on next launch.

### Task 3 — MetaFile defaults extended

`MetaFile::SetDefaultTypeSettings` for `AssetType::Texture` now also sets `wrap_mode=0`, `filter_min=0`, `filter_mag=0` alongside existing `generate_mipmaps=true`.

### Task 4 — TextureImporter reads .meta

`TextureImporter::Import()` loads the source `.meta`, parses the four settings fields into a `TextureSettings`, and stores it in `texData.Settings` before serializing the artifact.

### Task 5 — AssetManager passes settings

Both `LoadImmediate()` and `Update()` (async path) now call `Texture::Create(..., texData->Settings)` instead of the old no-settings overload. Added `AssetManager::Evict(UUID)` — removes an asset from the cache map so the next access triggers a fresh load.

### Task 6 — VulkanTexture GPU mip generation

**`VulkanTexture.h`:** Added `u32 m_MipLevels = 1`. `GetMipLevels()` returns `m_MipLevels`. New constructor `VKTexture(w, h, fmt, data, settings)`.

**`VulkanTexture.cpp`:**
- Two new helper functions: `ToVkWrapMode()`, `ToVkFilter()`.
- Format blit support check via `vkGetPhysicalDeviceFormatProperties` — falls back to `mipLevels=1` with a warning if `BLIT_SRC/DST_BIT` missing.
- `VK_IMAGE_USAGE_TRANSFER_SRC_BIT` added to color texture usage (required for blit source).
- Initial barrier transitions ALL mip levels to `TRANSFER_DST_OPTIMAL`.
- Mip generation loop (for `mipLevels > 1`): per-level barrier TRANSFER_DST→TRANSFER_SRC, `vkCmdBlitImage` halving dimensions (min 1), barrier TRANSFER_SRC→SHADER_READ_ONLY; final level TRANSFER_DST→SHADER_READ_ONLY.
- Sampler: `maxLod = (float)m_MipLevels`, `minLod = 0`, wrap/filter from settings.
- Image view: `levelCount = m_MipLevels`.

### Task 7 — Inspector texture panel

Fixed broken `static` combo state (was shared across all textures). Added `s_LastTextureUUID` guard — resets combo values when selection changes. Added **Generate Mipmaps** checkbox. Fixed filter combo order to match enum (`Linear=0, Nearest=1, ...`). **Apply** button now: loads `.meta`, writes all four settings, saves `.meta`, deletes artifact, calls `AssetManager::Import()` + `Evict()`, resets state for next frame.

### Files Modified

| File | Changes |
|---|---|
| `luth/renderer/Texture.h` | `TextureSettings` struct, new `Create` overload |
| `luth/renderer/Texture.cpp` | New `Create` overload → `VKTexture(..., settings)` |
| `luth/renderer/backend/vulkan/VulkanTexture.h` | `m_MipLevels`, updated `GetMipLevels()`, new constructor |
| `luth/renderer/backend/vulkan/VulkanTexture.cpp` | `ToVkWrapMode`, `ToVkFilter`, blit check, full mip generation, sampler maxLod, image view levelCount |
| `luth/resources/AssetSerializer.h` | `TextureHeader` + 4 settings fields |
| `luth/resources/AssetSerializer.cpp` | Serialize/deserialize new fields |
| `luth/resources/AssetManager.h` | `Evict(UUID)` declaration |
| `luth/resources/AssetManager.cpp` | `Evict()` impl, pass settings to `Texture::Create` (2 places) |
| `luth/resources/MetaFile.cpp` | `wrap_mode`, `filter_min`, `filter_mag` texture defaults |
| `luth/resources/importers/TextureImporter.h` | `TextureSettings Settings` in `TextureAssetData` |
| `luth/resources/importers/TextureImporter.cpp` | Read `.meta` into `TextureSettings` |
| `luth/editor/panels/InspectorPanel.cpp` | Per-texture state, Generate Mipmaps checkbox, Apply wired |
