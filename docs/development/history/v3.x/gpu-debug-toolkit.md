# gpu-debug-toolkit

**Date:** 2026-05-31
**Commits:** 3 (on `feat/rt-shadows`)
**Issue:** [#143](https://github.com/Hekbas/Luth/issues/143)
**Series:** adjacent to `rt-renderer` (Mode A). `Version.h` PATCH bumps `v3.0.9` → `v3.0.10`, tag-only,
no Release — internal diagnostic tooling.

---

## Overview

The skinned-mesh `VK_ERROR_DEVICE_LOST` on `feat/rt-shadows` (#140) was diagnosed with a pile of GPU
crash-debugging infrastructure assembled under time pressure: validation features, NV ray-tracing
validation, Nsight Aftermath, per-pass checkpoints, uncapped logging. It worked, but as built it had one
hard regression and several design gaps. This effort hardens it into a durable, documented subsystem and
commits it on its own issue + history + arch doc.

Three problems drove the work:

1. **Aftermath hard-crashed Debug.** premake link-time-linked `GFSDK_Aftermath_Lib.x64.lib` into *every*
   config, so any build needed the DLL at process start and crashed without it. **Root fix: dynamic load.**
2. **Validation was an always-on fixed bundle** (sync-val + GPU-AV + best-practices). GPU-AV's shader
   instrumentation + GPU→CPU readback perturb submit timing and *mask* races — the Heisenbug that hid
   this very bug. GPU-AV had to become opt-in, not default.
3. **Aftermath enabled `AUTOMATIC_CHECKPOINTS`** — NVIDIA rates it "very high CPU overhead" (a per-command
   call-stack walk) and it's redundant with our own per-pass `vkCmdSetCheckpointNV` markers.

The design was researched against how AAA engines expose validation (Unreal `-vulkanvalidation` /
`-gpuvalidation`, Godot `--gpu-validation` / `--gpu-abort`, Wicked's `ValidationMode`, The Forge), the
Khronos validation-layer settings surface, NVIDIA's per-flag Aftermath overhead guidance, and the
Aftermath SDK's dynamic-load typedefs. The normative reference is the new
[arch/gpu-crash-debugging.md](../../arch/gpu-crash-debugging.md).

---

## Sub-tasks

| # | What landed | Commit |
|---|---|---|
| 1 | **Dynamic-load Aftermath + drop link-time dep.** `premake5.lua` drops `libdirs` + `links` for `GFSDK_Aftermath_Lib.x64`, keeps the headers (`includedirs`), and bakes `LUTH_AFTERMATH_DLL="<sdk>/lib/x64/GFSDK_Aftermath_Lib.x64.dll"` (forward-slashed) alongside `LUTH_ENABLE_AFTERMATH=1`. `AftermathCrashTracker.cpp` resolves the three entry points (`Enable`/`Disable`/`GetCrashDumpStatus`) through the SDK's `PFN_GFSDK_Aftermath_*` typedefs after `LoadLibraryEx` (baked SDK path, then exe dir with hardened `LOAD_LIBRARY_SEARCH_*` flags). A missing DLL / missing entry point / version mismatch is a logged soft-fail — disabled, never a crash. | `7cbedee` |
| 2 | **Runtime validation tiers + checkpoint device-lost dump.** New `LUTH_VALIDATION` env knob (`ResolveValidationConfig`) replaces the never-shipped `LUTH_FORCE_VALIDATION`: `core` floor + opt-in `sync`/`bp`/`gpuav`/`rt`/`uncapped`, default Debug = `core+sync+bp`, `off` escape hatch. `CreateInstance` builds the `VkValidationFeatureEnableEXT[]` conditionally; GPU-AV logs a descriptor-budget/timing warning. NV-RT-validation gates on the `rt` tier. Aftermath diagnostics-config drops `AUTOMATIC_CHECKPOINTS` and only enables when `AftermathCrashTracker::Enabled()`. `GpuCheckpoint` registry + per-pass markers (`RenderGraph`) + `DumpCheckpointsOnDeviceLost` on all submit paths **and** `Present`, fire-once, with the TDR-unreliability note in code. | `20af16e` |
| 3 | **Arch doc + history + Version bump.** New normative `arch/gpu-crash-debugging.md` (the three layers + the device-lost playbook + the Future Work backlog). `arch/validation-layers.md` gains the runtime-tier section + cross-link and drops a stale `#elif defined(_DEBUG)` snippet. `.gitignore` adds the untracked runtime artifacts + Aftermath crash dumps. `Version.h` → `3.0.10`. | `de8f3f8` |
| 4 | **Stage the Aftermath DLL via post-build copy; drop the baked path.** The Runtime project copies `GFSDK_Aftermath_Lib.x64.dll` next to `Luthien.exe` on build (gated on `AFTERMATH_SDK`, mirroring `shaderc_shared.dll`); `AftermathCrashTracker` loads it by bare name. Drops the `LUTH_AFTERMATH_DLL` absolute-path `#define` so no env-specific path is baked into the compiled binary — it lives only in the gitignored vcxproj, like the Vulkan SDK path. *(Caught in smoke-test review.)* | this commit |

---

## Architectural decisions

### Dynamic load + post-build DLL staging

Two orthogonal choices. **How the code binds the DLL:** dynamic `LoadLibrary` + `GetProcAddress` (via the
SDK's `PFN_GFSDK_Aftermath_*` typedefs, "if dynamic loading is preferred") rather than a link-time import
lib. The symbols are undecorated `extern "C"` on x64, and dynamic binding makes a missing DLL, an old DLL
missing an entry point, and a header/lib version mismatch all soft-fail through the same path — disabled,
never a crash. The headers stay compile-time-only (`includedirs`); only `AFTERMATH_SDK` at generate time
toggles `LUTH_ENABLE_AFTERMATH`.

**How the DLL reaches the exe:** the Runtime project's post-build step copies it next to `Luthien.exe`,
exactly as it already does for `shaderc_shared.dll`, and the runtime loads it by bare name with hardened
search (`LOAD_LIBRARY_SEARCH_APPLICATION_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS` — never the CWD /
`%PATH%`). The first cut instead baked the absolute SDK path into the binary as a `LUTH_AFTERMATH_DLL`
define; it worked but embedded a machine-specific path in the compiled artifact, so it was dropped for the
post-build copy — the absolute path now lives only in the gitignored vcxproj, like the Vulkan SDK path.

### One env knob, two-axis model, legacy feature route

`LUTH_VALIDATION` is a single comma-list knob, not a tier-level integer (Wicked's `Disabled < Enabled <
GPU < Verbose` ladder) — because an ordinal ladder re-introduces the exact conflation that bit us: it
collapses "GPU-AV" and "more severity" onto one axis. Following Unreal, feature tiers (`core`/`sync`/
`bp`/`gpuav`/`rt`) are kept orthogonal to severity (still fixed at `WARNING|ERROR` in the messenger). The
default Debug tier deliberately **excludes** `gpuav` so a race stays observable; the value forces
validation on in *any* build, which is how a release-only fault gets inspected without a recompile.

Feature selection stays on the legacy `VkValidationFeaturesEXT` route even though it was deprecated at
Vulkan header 272 (superseded by `VK_EXT_layer_settings`). The legacy route still works, hits the effort's
goal with minimal churn, and `uncapped` already uses layer-settings for `duplicate_message_limit`. The
migration — which would unlock the granular `gpuav_*` sub-toggles (e.g. instrument only RT shaders) — is
recorded as Future Work, not done here. The arch doc also documents that **CI can drive every tier through
raw `VK_KHRONOS_VALIDATION_*` env vars with zero engine code** (loader-level), which is the real long-term
win and the reason a migration isn't urgent.

### Drop `AUTOMATIC_CHECKPOINTS`; keep the cheap three

NVIDIA's own guidance rates the four `VK_DEVICE_DIAGNOSTICS_CONFIG` flags very differently:
`SHADER_ERROR_REPORTING` (zero), `RESOURCE_TRACKING` (low, no per-cmd cost), `SHADER_DEBUG_INFO`
(compile-time + memory), and `AUTOMATIC_CHECKPOINTS` (**very high CPU** — a call-stack walk per
draw/dispatch/copy). The first three named the faulting resource + shader for the skinned bug;
`AUTOMATIC_CHECKPOINTS` contributed nothing and is redundant with the per-pass `vkCmdSetCheckpointNV`
markers, which localize the failing pass for negligible cost. Dropped. The diagnostics-config extension
is now also gated on `AftermathCrashTracker::Enabled()` so a soft-fail (no DLL) incurs no driver-side
tracking overhead at all.

### Checkpoints stay extension-gated; dump extended to Present

The per-pass marker is cheap enough to leave on whenever the device advertises the extension, in any
non-Dist dev build, so no tier wiring was added for it. The device-lost dump already fired from the three
submit paths; `Present` was added because a TDR can first surface at `vkQueuePresentKHR`. The dump is
fire-once (every post-loss submit returns `-4`), so the extra call site is free. A code comment + the arch
doc flag the **TDR-unreliability** of checkpoints (markers are wiped by the GPU reset — "no checkpoints
recorded" is expected), which is why the basic checkpoint dump was uninformative during the original hunt.

### Composition with existing primitives (cornerstone check)

No new allocator, ring buffer, or sync primitive. `GpuCheckpointRegistry` interns into a
`std::unordered_set` under `Luth::SpinLock` (V1); Aftermath callbacks write files only at crash time (not
a hot path). The device-lost dump iterates the existing graphics/compute/transfer queues via
`m_ComputeIsAsync`/`m_TransferIsAsync` (multi-queue.md). Validation composes with `m_EnableValidationLayers`,
`CheckValidationLayerSupport()` soft-fail, and `LUTH_ENABLE_VALIDATION` (BuildConfig.h). The one genuinely
new mechanism — Win32 `LoadLibraryEx`/`GetProcAddress` in `AftermathCrashTracker.cpp` — has no existing
engine primitive (all dynamic-load sites are in `extern/`) and is justified by the SDK's dynamic-load
typedefs + the soft-fail requirement; isolated to one Windows-only, `LUTH_ENABLE_AFTERMATH`-gated file.

---

## Files touched

**Engine (Luth.lib):**
- [`AftermathCrashTracker.{h,cpp}`](../../../luth/source/luth/renderer/backend/vulkan/AftermathCrashTracker.cpp) — dynamic-load rewrite (the `.h` interface is unchanged)
- [`GpuCheckpoint.{h,cpp}`](../../../luth/source/luth/renderer/backend/vulkan/GpuCheckpoint.h) — checkpoint marker name registry (new in the hunt; committed here)
- [`VulkanContext.{h,cpp}`](../../../luth/source/luth/renderer/backend/vulkan/VulkanContext.cpp) — `ResolveValidationConfig` + tiers, conditional feature array, RT-validation gating, Aftermath flag trim + `Enabled()` gate, checkpoint fn-load + `DumpCheckpointsOnDeviceLost` + Present hook
- [`RenderGraph.cpp`](../../../luth/source/luth/renderer/rendergraph/RenderGraph.cpp) — per-pass `vkCmdSetCheckpointNV` markers (interned pass names)
- [`premake5.lua`](../../../luth/premake5.lua) — Aftermath: drop the `.lib` link (dynamic-load instead)
- [`runtime/premake5.lua`](../../../runtime/premake5.lua) — post-build copy of the Aftermath DLL next to the exe

**Docs:**
- [`arch/gpu-crash-debugging.md`](../../arch/gpu-crash-debugging.md) — new normative reference
- [`arch/validation-layers.md`](../../arch/validation-layers.md) — runtime-tier section + cross-link + stale-snippet fix
- `.gitignore` — runtime layout caches + Aftermath crash artifacts

---

## Verification

- Debug + Release + Dist x64 build clean (with `AFTERMATH_SDK` set → real dynamic-load path, and without
  → compiled-out stub). Only pre-existing warnings (C4267 size_t, C4996 `getenv`/`strncpy`/`sscanf`, C4244
  chrono, LNK4006 import-descriptor duplicates).
- **Debug with no Aftermath DLL present launches cleanly** — soft-fail log line, no crash (the regression
  fixed). *(runtime smoke-test)*
- `LUTH_VALIDATION` matrix: unset Debug = `core+sync+bp` clean; `LUTH_VALIDATION=gpuav` enables GPU-AV +
  prints the descriptor/timing guardrail; `LUTH_VALIDATION=off` disables in Debug; `LUTH_VALIDATION=sync,rt`
  (+ `NV_ALLOW_RAYTRACING_VALIDATION=1`) enables RT validation.
- Aftermath opt-in (`AFTERMATH_SDK` built + DLL present): startup logs "enabled"; a forced device-lost
  writes `luth_gpucrash_0.nv-gpudmp` (opens in Nsight Graphics). *(optional)*
- Re-confirmed the underlying fix holds: Release skinned `Walking.luth`, RT + CSM, no device-lost.
