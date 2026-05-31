# Vulkan Validation Layers

## Policy

Validation layers are **enabled by default in Debug**, **disabled by default in Release/Dist**, with a build-define override. Owned entirely by `VulkanContext` ([luth/source/luth/renderer/backend/vulkan/VulkanContext.{h,cpp}](../../../luth/source/luth/renderer/backend/vulkan/VulkanContext.h)).

---

## Enable / disable

### Build-config defaults

| Config | `_DEBUG` defined | `m_EnableValidationLayers` |
|--------|------------------|----------------------------|
| Debug   | yes | `true`  |
| Release | no  | `false` |
| Dist    | no  | `false` |

### Build-define override

Define `LUTH_ENABLE_VALIDATION` to force the value, regardless of build config:

| Compile flag | Effect |
|--------------|--------|
| `-DLUTH_ENABLE_VALIDATION=1` | Validation forced **on** (use to validate a Release build) |
| `-DLUTH_ENABLE_VALIDATION=0` | Validation forced **off** (use to disable in Debug for a perf-sensitive capture) |
| (undefined) | Falls back to `_DEBUG` autoselect |

`LUTH_ENABLE_VALIDATION` is always defined by [`BuildConfig.h`](../../../luth/source/luth/core/BuildConfig.h)
(1 in Debug, 0 in Release/Dist, overridable per-config in premake), so the member is a single branchless line in
[`VulkanContext.h`](../../../luth/source/luth/renderer/backend/vulkan/VulkanContext.h):

```cpp
bool m_EnableValidationLayers = (LUTH_ENABLE_VALIDATION != 0);
```

Engine code never tests `_DEBUG` / `NDEBUG` directly — those are toolchain artifacts (see `BuildConfig.h`).

---

## Runtime override — `LUTH_VALIDATION`

The `LUTH_VALIDATION` **environment variable** overrides the build default in *any* build (no recompile) and
selects which feature tiers run — the mechanism for diagnosing **release-only** GPU faults. Unset honors the
build-config default above; `off`/`0`/`none` forces validation off even in Debug; any other value forces it
on. The default tier is `core` + sync-val + best-practices; **GPU-AV is opt-in only** (it perturbs submit
timing and can mask races). `ResolveValidationConfig()` parses it before `vkCreateInstance`.

Full toolkit — tiers, GPU checkpoints, Nsight Aftermath, and the device-lost playbook — is the normative
[arch/gpu-crash-debugging.md](gpu-crash-debugging.md). Validation is layer 1 of three.

---

## Layer requested

Single layer: `VK_LAYER_KHRONOS_validation` (the Khronos meta-layer that bundles all validation features).

`CheckValidationLayerSupport()` ([VulkanContext.cpp:264–282](../../../luth/source/luth/renderer/backend/vulkan/VulkanContext.cpp)) enumerates `vkEnumerateInstanceLayerProperties` and verifies the layer is present **before** instance creation. If requested but missing (e.g. Vulkan SDK not installed on the dev machine), `m_EnableValidationLayers` is forced to `false` and a `LH_CORE_ERROR` is logged. Instance creation continues without validation rather than crashing.

---

## Debug messenger

When validation is enabled, `SetupDebugMessenger()` registers a `VkDebugUtilsMessengerEXT` via `vkCreateDebugUtilsMessengerEXT` (loaded with `vkGetInstanceProcAddr`).

### Severity filter

Listens for: `VERBOSE | WARNING | ERROR` (skips `INFO`, which is too chatty).

### Message types

`GENERAL | VALIDATION | PERFORMANCE` — all three categories.

### Callback routing

`DebugCallback()` in [`VulkanContext.cpp:12–24`](../../../luth/source/luth/renderer/backend/vulkan/VulkanContext.cpp) maps Vulkan severity to engine log levels:

| Vulkan severity | Engine log |
|-----------------|------------|
| `ERROR_BIT_EXT` | `LH_CORE_ERROR` |
| `WARNING_BIT_EXT` | `LH_CORE_WARN` |
| `VERBOSE_BIT_EXT` (and below) | dropped |

Returns `VK_FALSE` so Vulkan continues normal execution after the callback (the standard pattern — returning `VK_TRUE` aborts the call that triggered the message, which is reserved for layer-development scenarios).

### Cleanup

`Shutdown()` calls `vkDestroyDebugUtilsMessengerEXT` (loaded the same way) before `vkDestroyInstance`. Skipped when validation is disabled.

---

## When to override

| Scenario | Action |
|----------|--------|
| Profiling a Release build and want to confirm no validation errors | `-DLUTH_ENABLE_VALIDATION=1` for a one-off run |
| Investigating a crash where validation noise is hiding the real issue | `-DLUTH_ENABLE_VALIDATION=0` in Debug |
| Capturing a Tracy frame with no validation overhead | `-DLUTH_ENABLE_VALIDATION=0` in Debug, or use a Release build |
| Distributing the engine | Dist build — validation is auto-off |

---

## Notes

- No `VK_LAYER_LUNARG_*` or `VK_LAYER_GOOGLE_*` layers are requested. The Khronos meta-layer covers everything LunarG/GOOGLE used to provide separately.
- Loader path: ensure the Vulkan SDK is installed on the dev machine — `VK_LAYER_KHRONOS_validation` ships with the SDK, not with the runtime drivers.
- The callback writes to the engine log stream (`LH_CORE_*`), so validation messages flow through the same sinks as normal engine logging (Console panel, file sink).
- Per-message-id suppression is **not** implemented. If a known false-positive becomes noisy, add a filter in `DebugCallback` keyed on `pCallbackData->pMessageIdName`.
