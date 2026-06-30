# GPU Crash Debugging

How Luth diagnoses GPU faults — especially `VK_ERROR_DEVICE_LOST` (TDR). Three independent layers,
ordered by when they fire, plus the playbook for working a device-lost. Owned by `VulkanContext`
([VulkanContext.{h,cpp}](../../../luth/source/luth/renderer/backend/vulkan/VulkanContext.h)),
`GpuCheckpoint.{h,cpp}`, and `AftermathCrashTracker.{h,cpp}` in the Vulkan backend.

| Layer | When | Catches | Cost |
|---|---|---|---|
| **Validation** (CPU layer) | pre-crash, live | missing barriers, bad usage, shader OOB | moderate–high |
| **Checkpoints** (GPU breadcrumb) | at TDR | *which pass* was executing — when they survive | negligible |
| **Aftermath** (post-mortem dump) | at TDR | faulting shader + resource + page-fault VA | dev-only |

The cornerstone: validation is the *first* line (run it continuously), Aftermath is the *post-mortem
of record* for a device-lost, and checkpoints are a cheap breadcrumb fallback — not a primary tool.

---

## 1. Validation tiers — `LUTH_VALIDATION`

Validation is gated by `LUTH_ENABLE_VALIDATION` (build default: on in Debug, off in Release/Dist — see
[validation-layers.md](validation-layers.md) + `BuildConfig.h`). The **`LUTH_VALIDATION` environment
variable** overrides that default in *any* build and selects which feature tiers run. This is the key
to diagnosing **release-only** faults: the validation layer can be turned on in a Release binary at
startup, no recompile.

### Two axes, decoupled

The bug that motivated this toolkit was masked because the old code conflated "validation on" with "GPU-AV
on" — and GPU-AV's shader instrumentation + GPU→CPU readback perturb submit timing enough to hide a race
(a Heisenbug). Following the Unreal/Godot/Wicked convention, **feature tiers** are separated from
**severity**:

- **Feature tiers** (`LUTH_VALIDATION` value) — *which* checks run. `core` is the always-on floor.
- **Severity** — *how loud*. Today fixed at `WARNING | ERROR` in the debug messenger (see
  validation-layers.md). A runtime severity axis is future work; not needed yet.

### Values

| `LUTH_VALIDATION` | Tiers enabled |
|---|---|
| *unset* | build default (Debug: `core` + `sync` + `bp`; Release/Dist: off) |
| `1` / `on` / `default` / *empty* | `core` + `sync` + `bp` |
| `off` / `0` / `none` | force off, even in Debug |
| token list (`,`/`;`/space) | exactly those, `core` always implied |
| `all` | `core` + `sync` + `bp` + `gpuav` + `rt` + `uncapped` |

Tokens: `sync` (synchronization validation), `bp`/`bestpractices`, `gpuav`/`gpu` (GPU-assisted, pairs
with reserve-binding-slot), `rt` (NV ray-tracing validation), `uncapped`/`verbose` (lift the duplicate-
message cap to unlimited).

### Why GPU-AV is opt-in, never default

GPU-AV (`gpuav`) is excluded from every default tier on purpose:

- **It masks races.** Instrumentation + readback overhead shift timing — exactly what can hide a
  release-only hazard. Run a suspected race *without* GPU-AV first.
- **It consumes a descriptor set.** GPU-AV reduces `maxBoundDescriptorSets` by one. Post-`bindless-migration`
  Luth packs descriptor sets — if at the cap, GPU-AV silently no-ops. `CreateInstance` logs a one-line
  warning whenever `gpuav` is enabled.
- **Don't combine with core needlessly.** LunarG recommends running Core and GPU-AV separately (cost
  multiplies). `gpuav` is for a focused shader-OOB / bad-device-address hunt.

`rt` (NV ray-tracing validation) is likewise opt-in: it reports through the debug-utils messenger (so it
forces the validation layer on) and the driver only exposes the extension when the external
`NV_ALLOW_RAYTRACING_VALIDATION=1` environment variable is *also* set.

### Mechanism + the deprecation note

`ResolveValidationConfig()` parses `LUTH_VALIDATION` → `m_EnableValidationLayers` + `m_ValTiers`.
`CreateInstance` then builds a conditional `VkValidationFeatureEnableEXT[]` array and chains a
`VkValidationFeaturesEXT` into the instance `pNext`. `uncapped` additionally probes for `VK_EXT_layer_settings`
and sets `duplicate_message_limit = 0`.

> **`VkValidationFeaturesEXT` is deprecated (Vulkan header 272)** in favor of `VK_EXT_layer_settings`
> keys (`validate_sync`, `gpuav_enable`, `validate_best_practices`). The legacy route still works and is
> kept for minimal churn. Migrating *feature* control to layer-settings is future work — it unlocks the
> granular `gpuav_*` sub-toggles (e.g. `gpuav_select_instrumented_shaders` to instrument only RT shaders).

**CI angle (the long-term win).** Every tier is also drivable at the loader level via the raw Khronos
env vars — `VK_LOADER_LAYERS_ENABLE=*validation`, `VK_KHRONOS_VALIDATION_VALIDATE_SYNC=true`,
`VK_KHRONOS_VALIDATION_GPUAV_ENABLE=true`, etc. — with **zero engine code**. A CI job can boot the engine
under sync-val and fail on any validation message without touching `LUTH_VALIDATION`. See Future Work.

---

## 2. Checkpoints — `VK_NV_device_diagnostic_checkpoints`

A GPU breadcrumb: `RenderGraph` emits `vkCmdSetCheckpointNV` per pass (marker = the interned pass name's
`c_str()`, stable for process lifetime — `GpuCheckpointRegistry`). On `VK_ERROR_DEVICE_LOST`,
`DumpCheckpointsOnDeviceLost` walks each queue's `vkGetQueueCheckpointDataNV` and logs the last-executed
marker per queue. Enabled whenever the device advertises the extension; the per-pass marker is negligible.

The dump fires from every submit path (`SubmitGraphics2`/`SubmitCompute2`/`SubmitTransfer2`) **and**
`Present` (a TDR can first surface at present), gated **fire-once** — once the device is lost every
subsequent submit returns `-4`, and only the first dump is informative.

> **invariant: checkpoints are TDR-unreliable.** The markers are frequently wiped by the GPU reset, so
> "no checkpoints recorded" is the *expected* outcome, not a bug. Treat the checkpoint dump as a fallback;
> Aftermath (below) is the post-mortem of record. This is why the original basic-checkpoint dump showed
> nothing for the skinned device-lost.

---

## 3. Aftermath — `VK_NV_device_diagnostics_config` + crash dumps

NVIDIA Nsight Aftermath is the gold standard for a device-lost: it survives the TDR and, with resource
tracking, names the **faulting resource** (size, format, page-fault VA) and the **faulting shader**.
Synchronization validation cannot track acceleration-structure or buffer-device-address accesses —
Aftermath sees both.

### Compile gate + dynamic load

`AftermathCrashTracker` is compiled in only when premake sees the `AFTERMATH_SDK` env var at generate
time (`LUTH_ENABLE_AFTERMATH=1` + the include dir); the SDK headers are compile-time only, and the
Runtime project's post-build step copies the DLL next to `Luthien.exe` (mirroring the Slang compiler DLLs).
**The DLL is loaded dynamically at runtime** (`LoadLibraryEx` + `GetProcAddress` via the SDK's
`PFN_GFSDK_Aftermath_*` typedefs) — never link-time-linked. A missing DLL, missing entry points, or
version mismatch is a logged soft-fail: disabled, **never a crash**.

> The link-time predecessor hard-crashed any config that linked the import lib (Debug included) when the
> DLL was absent. Dynamic load is the root fix.

Loaded by name from the exe directory — **no absolute SDK path is baked into the binary** (the post-build
copy stages it, like the Slang compiler DLLs). Hardened `LOAD_LIBRARY_SEARCH_*` flags keep a planted
DLL on the CWD / `%PATH%` from being picked up.

### Init order + flags

`AftermathCrashTracker::Initialize()` runs **before** `vkCreateInstance` (required — no dumps for devices
created earlier). At device creation, `VK_NV_device_diagnostics_config` is enabled **only if Aftermath
actually loaded** (`Enabled()`), so a soft-fail incurs no driver-side tracking overhead. Flags, with
NVIDIA's overhead rating:

| Flag | What it adds to the dump | Cost | Enabled |
|---|---|---|---|
| `SHADER_ERROR_REPORTING` | faulting shader / exception type | **zero** | yes |
| `RESOURCE_TRACKING` | faulting resource behind a page-fault VA | low (no per-cmd) | yes |
| `SHADER_DEBUG_INFO` | source-line mapping (`.nvdbg`) | compile-time + memory | yes (dev) |
| `AUTOMATIC_CHECKPOINTS` | per-command CPU call stack | **very high CPU** | **no** |

`AUTOMATIC_CHECKPOINTS` is deliberately **dropped**: it walks the recording thread's call stack per
draw/dispatch/copy, and the per-pass `vkCmdSetCheckpointNV` markers in §2 already localize the failing
pass far cheaper. On a TDR, `OnDeviceLost()` polls (bounded ~5 s) until the dump finishes and writes
`luth_gpucrash_N.nv-gpudmp` (+ `luth_shaderdbg_N.nvdbg`). Open the `.nv-gpudmp` in **Nsight Graphics**.

Callbacks are free-threaded but fire only at crash time — not a hot path, so the file-write there is fine.

---

## Device-lost playbook

Worked from the real `BoneMatrixBuffer` use-after-free (skinned mesh, Release-only, both RT and CSM shadow
modes — a tagged-heap region freed one frame too early, read by the skinned vertex shader → MMU page fault):

1. **Repro under `LUTH_VALIDATION=sync,bp`** (default Debug). Sync-val catches most missing-barrier and
   cross-queue hazards cheaply. *If it reproduces and names a hazard, you're done.*
2. **If it's release-only or sync-val is silent**, force validation on in the failing config:
   `LUTH_VALIDATION=sync` in Release. Beware: **don't** reach for `gpuav` first — it can mask the race.
   Sync-val **cannot** track acceleration-structure / buffer-device-address accesses, so a BDA-fed buffer
   or BLAS race is invisible to it (this is why days of validation found nothing here).
3. **Bring in Aftermath** (build with `AFTERMATH_SDK`, DLL present). On the device-lost it names the
   faulting resource + shader + page-fault VA — decisive for a device-lost. Here it named the 4 MiB
   `BoneMatrixBuffer` faulting in a vertex shader on the graphics queue.
4. **Checkpoints last** — they localize the *pass* when they survive the reset, but expect "no checkpoints
   recorded" (TDR-wiped). A breadcrumb, not the answer.
5. Only reach for **`gpuav`** for a shader-side OOB / bad-device-address hunt where the instrumentation's
   timing perturbation is acceptable.

---

## Future Work

| Item | Why | Notes |
|---|---|---|
| **CI validation job** | biggest long-term win — catch sync hazards continuously | Tier-1: lavapipe headless (`VK_EXT_headless_surface`), `sync` + `bp`, per-PR, GPU-free (Windows via `jakoch/install-vulkan-sdk-action`). Tier-2: self-hosted NVIDIA GPU, adds `gpuav`, nightly. Harness: `VkDebugUtilsMessenger` callback that **returns `VK_FALSE` always** (never `VK_TRUE` — it injects `VK_ERROR_VALIDATION_FAILED_EXT`), sets an atomic flag on ERROR, renders N deterministic frames, `vkDeviceWaitIdle`, exits non-zero. VUID allowlist in-callback. |
| **Fix pre-existing sync-val hazards** | real, surfaced during the hunt (static-RT tolerates them) | cross-queue `WRITE_AFTER_WRITE` (graphics↔compute layout transitions on RT/GTAO/volumetric images); swapchain `WRITE_AFTER_READ` (acquire vs first transition); `loadOp LOAD` attachment read-after-write in the RG barrier solver; a GPU-AV "uninitialized descriptor" (set 0 b0 image sample) in an editor-startup compute pass. |
| **In-editor toggles** | dev convenience | validation tiers + an Aftermath "dump now" / device-lost affordance. Needs a device recreate (validation is decided at instance/device creation, before the editor/settings exist), which is why env-var is the runtime mechanism today. |
| **`VK_EXT_device_fault`** | portable AMD/Intel/NVIDIA device-lost info | cheap fast-follow — queried only *after* `DEVICE_LOST` (zero standing cost). `vkGetDeviceFaultInfoEXT` gives fault descriptions + page-fault VAs cross-vendor; complements NVIDIA-only Aftermath. |
| **`VK_EXT_layer_settings` migration** | unlocks `gpuav_*` granular toggles | replace the deprecated `VkValidationFeaturesEXT` route; `gpuav_select_instrumented_shaders` would cut GPU-AV cost on an RT engine by instrumenting only RT shaders. |
| **Dist Aftermath path** | field crash-dump collection | ship-safe minimal flag set is `RESOURCE_TRACKING | SHADER_ERROR_REPORTING` (both ~zero overhead), in-process collector, DLL soft-loaded + redistributed. Requires per-build `.nvdbg` symbol archival for offline symbolication. Defer until shipping. |
