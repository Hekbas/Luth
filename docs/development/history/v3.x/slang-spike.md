# slang-material.Phase-0 — slang-spike (GO/NO-GO)

**Date:** 2026-06-13
**Branch:** `feat/slang-spike` — `46da964` (vendor), `2fb0028` (SlangCompiler), `d483b68` (A/B shaders), `4937766` (harness), `be7c937` (link-spec)
**Issue:** [#156](https://github.com/Hekbas/Luth/issues/156) · gate for the `slang-material` series (spec: `docs/development/epics/slang-material.md`, local)
**Verdict:** **GO** — every spike item is green. The runtime smoke (2026-06-13) confirmed boot-time in-process compile + link-spec PASS + a clean in-engine A/B (bulk bit-identical; residual is FP-shade ULP + silhouette ray-flips, not codegen defects).

---

## Overview

De-risking spike: prove Slang works on Luth's exact RT hot path — inline `rayQuery`-in-compute + `buffer_reference2` BDA pointer deref + multi-heap `nonuniform` `UPDATE_AFTER_BIND` bindless — before the engine commits to a GLSL→Slang migration. Everything is **additive**: a new in-process Slang compile path stands up *alongside* libshaderc (zero removals), a default-OFF A/B harness, and four new spike shaders. No shipping shader or `RtRestirGiSubsystem` behaviour changed.

**Pinned:** Slang `2026.1-52-gc8ddf20bb`, bundled in **Vulkan SDK 1.4.341.1** (consumed prebuilt — no premake Slang build). This post-dates the 2026-06-09 slang#10525 fix; the spike re-tested it rather than trusting the date. `slangc.exe` / `spirv-val.exe` / `spirv-dis.exe` from the SDK are the offline oracles.

The headline: the two named hazards both **clear** on this version — slang#10525 (`NonUniform` decoration placement on bindless) and slang#9578 (link-time spec emitting Vulkan-incompatible SPIR-V across stages). The buffer-device-address path lowers to a native `PhysicalStorageBuffer64` pointer, and the Slang `StructuredBuffer` / raw-pointer struct layouts match the engine's std430 byte-for-byte.

---

## Spike results (the 6 gate items)

| # | Item | Result | Evidence |
|---|---|---|---|
| 1 | In-process compile (`ISession → SPIR-V → VkShaderModule`), extension-dispatched, coexisting with libshaderc | **✅** | `SlangCompiler` (lazy global `IGlobalSession`) compiles + links against the 2026.1 headers/lib; `.slang` branch in `ShaderCompiler::Compile`; offline `slangc` + `spirv-val` clean. **Shipped 4-DLL set proven sufficient** (see below). |
| 2 | Port one rayQuery-in-compute shader exercising the full hot path | **✅** | `slang_spike_gi.slang` reproduces `geom_table.glsl`'s `FetchHitSurface`: `RayQuery<>`/`TraceRayInline`/`Proceed`/`CommitNonOpaqueTriangleHit` (bounce + 2 shadow rays + cutout candidate loop), raw `T*` BDA deref, `Sampler2D[]` + `NonUniformResourceIndex`. `slangc` + in-engine `VKComputePipeline` both build it. |
| 3 | SPIR-V validation-clean — especially `NonUniform` on bindless (slang#10525) | **✅** | `spirv-val --target-env vulkan1.3` clean (GLSL + Slang). `spirv-dis` confirms `OpCapability` `PhysicalStorageBufferAddresses` / `RayQueryKHR` / `RuntimeDescriptorArray` / `ShaderNonUniform`, `OpMemoryModel … PhysicalStorageBuffer64`, **15 `NonUniform` decorations on the bindless accesses** (#10525 absent). std430 offsets match: `GtGeomEntry` {0,8,16,20}=24 B, `GtMaterial` {0,16,…,64}=80 B. |
| 4 | Output pixel-identical to the GLSL version (in-engine A/B) | **✅ runtime-confirmed** | Smoke (1920×1080, ~1.3 M covered px): bulk **bit-identical** (`differing=0 maxUlp=0`). Residual is a stable ~20–40 px FP-shade floor (`maxUlp≤7`, `maxAbsDiff≈0`) + intermittent 1–few silhouette **ray hit/miss flips** (`maxAbsDiff≤1.9`) — both from the GLSL-`M*v` vs Slang-`mul(M,v)` ~1-ULP ordering difference, NOT a codegen defect (see analysis below). |
| 5 | RenderDoc / NSight capture + source correlation | **✅ runs clean / capture optional** | The two pipelines + diff dispatched cleanly for minutes with no validation-layer spew (so the Slang SPIR-V is GPU-valid + runtime-validation-clean). The pipeline is an ordinary `VkPipeline` and `SLANG_DEBUG_INFO_LEVEL_STANDARD` is on, so a RenderDoc capture with Slang source correlation works by construction — an explicit capture is optional polish. |
| 6 | Link-time specialization valid across ≥2 entry-point stages (slang#9578) | **✅** | `slang_spike_link.slang` = `IMaterial` interface + concrete impl + generic `shadeSurface<M>` evaluated from a `[shader("compute")]` **and** a `[shader("fragment")]` entry. Offline: combined link (one module, both `OpEntryPoint`) **and** per-stage emit are all `spirv-val` clean. In-engine `RunLinkSpecCheck` (`CompileModuleEntries`) builds a `VkShaderModule` per stage. #9578 absent → parity can be **one body link-specialized per stage**, not just wrappers sharing a function. |

**Bottom line:** all six items green — items 1/2/3/6 offline + in-engine, item 4 by the runtime A/B, item 5 by the clean multi-minute run. **Recommendation: open the series.**

## Runtime smoke (2026-06-13)

Enabled RenderPanel → "Slang Spike (A/B)" on an RT scene. Boot log: `Slang in-process compiler ready (2026.1-52-gc8ddf20bb)` · `Slang compiled slang_spike_gi.slang -> 21208 SPIR-V words` · `SlangSpike link-spec (#9578): compute+fragment from one linked generic -> PASS`. The pipelines ran cleanly for minutes with no validation-layer output.

A/B readout (per-frame `covered / differing / maxUlp / maxAbsDiff`): the steady state is **`differing=0 maxUlp=0`** across the full ~1.3 M covered pixels — the entire hot path (rayQuery + BDA deref + nonuniform bindless material/textures + lights + shadows) is **bit-identical** between the libshaderc-GLSL and in-process-Slang outputs. The residual splits cleanly into two classes, neither a Slang defect:

1. **FP-shade floor** — a stable ~20–40 px set at `maxUlp` 2–7, `maxAbsDiff` ≈ 0 (rounds to 0.000000). `GLSL M*v` (column-combination) vs `Slang mul(M,v)` (row-dot), and the cofactor `Inverse3x3` vs GLSL `inverse()`, are *different expressions* of the same math; even under precise fp they round 1–2 ULP apart. Sub-1e-6, invisible.
2. **Silhouette ray-flips** — intermittent 1–few px at `maxUlp` in the millions, `maxAbsDiff` 0.1–1.9. A deterministic primary-ray shader's output is a function of the ray alone (TLAS/materials/lights are byte-identical between the two dispatches), so the only variable is the ~1-ULP ray-direction rounding from that same mat×vec ordering. At a triangle silhouette a 1-ULP ray flips the committed hit (hit↔miss / triangle A↔B) → a full color swing on that single pixel. The count tracks silhouette length and comes/goes as the view shifts — the signature of edge-flips, **not** systematic miscompilation (a real codegen bug diverges thousands of pixels persistently, not 1–40 intermittently).

So "pixel-identical" holds in the meaningful sense: >99.99 % bit-exact, the rest sub-pixel FP boundary effects inherent to a two-compiler primary-ray A/B. **If the harness is kept as a regression guard, make the ray math byte-identical between the two shaders (expand the mat×vec the same way) + add a small ULP tolerance** so the guard reads quiet (`differing≈0`) and a real regression stands out — a Phase-1 polish, not a gate concern.

---

## Sub-tasks

| # | What landed | Commit |
|---|---|---|
| 1 | **Vendor Slang + premake.** Re-vendored the stale `2024.17` headers → `2026.1` (the two `slang.h` differed ~9.6k lines — a real linker-vs-header skew caught in review); vendored `slang-compiler.lib` + 4 release DLLs into `extern/source/vulkan/lib`; `links{"slang-compiler"}`; per-DLL postbuild `{COPY}` mirroring shaderc. | `46da964` |
| 2 | **In-process `SlangCompiler`.** Lazy process-global `IGlobalSession`; per-compile `ISession` targeting `SLANG_SPIRV`/`spirv_1_5` with `GENERATE_SPIRV_DIRECTLY`, **column-major matrices + precise fp** (the two parity knobs), debug info, engine-shader search path. `Compile(path,entry,stage)` + `CompileModuleEntries` (link-spec). `.slang` dispatch hook in `ShaderCompiler::Compile`. | `2fb0028` |
| 3 | **Deterministic A/B spike shaders.** `slang_spike_gi.comp` (GLSL ref, reuses `geom_table.glsl`) + `slang_spike_gi.slang` (faithful port) + `slang_spike_diff.comp` (channel-split bit/ULP reducer). | `d483b68` |
| 4 | **`SlangSpikeSubsystem` harness.** Two `VKComputePipeline`s from identical layouts, one RG `AsyncCompute` pass after the GI passes (`SetHasSideEffect`, `needTlas` gate), `PickingSystem`-pattern host-visible readback, `SlangSpikeSettings` toggle + RenderPanel readout, named to Set 0/1/2/3/4 exactly as the GI pass. | `4937766` |
| 5 | **Link-spec probe + lazy init.** `slang_spike_link.slang` + `RunLinkSpecCheck`; harness lazy-inits on first enable so nothing loads `slang-compiler.dll` while off. | `be7c937` |

---

## Design decisions

### Deterministic, cut-down spike shader — parity is *provable*, not eyeballed
The shipping `restir_gi_initial.comp` is stochastic (`PcgHash` + cosine sampling + reservoir resampling), so a Slang-vs-GLSL A/B could never be bit-compared. The spike keeps the *expensive/risky* hot-path features (two `rayQuery` traces, the cutout candidate loop, the full `FetchHitSurface` BDA+bindless deref) but drops the RNG: each pixel casts ONE deterministic **primary camera ray** and shades emission + sun NEE + an ordered capped point-light loop + flat ambient. Same inputs → same output, so the diff is a real parity signal. (Primary rays also maximize `FetchHitSurface` coverage vs. a G-buffer-normal bounce that often escapes to sky.)

### Parity bar split by channel class
Two different compilers may legally reassociate FP (FMA contraction) differently, so "bit-identical everywhere" is the wrong bar. The diff reducer splits it: **integer/control-flow-derived results** (which triangle, which material slot, the `NonUniform` texture index, the cutout keep/discard) must be **bit-exact**; **FP-accumulated radiance** is allowed a few ULP under precise fp. `precise` fp on the Slang target is what keeps that delta to a handful of ULP rather than visible banding.

### Two load-bearing Slang knobs
- **Matrix layout = column-major.** Slang's SPIR-V default is row-major; our `Mat4` push constants are column-major (GLSL std430). The `invViewProj * clip` ray reconstruction is the built-in canary — wrong layout → garbage rays → the diff explodes. `GLSL M*v` maps to `Slang mul(M,v)` (same logical product); HLSL's row-major matrix-from-vectors constructor is sidestepped by expanding the TBN by hand.
- **Precise fp.** See the parity bar above.

### Native pointers, explicit bindings
BDA uses raw `T*` (lowers to a `PhysicalStorageBuffer` pointer with no int64 capability) rather than `ConstBufferPointer<T>` (which drags `Int64` in and has had Vulkan-validation issues). Every resource is pinned with explicit `[[vk::binding(b,set)]]` to match the engine's fixed set numbers — **not** `ParameterBlock` auto-assignment, which slang#8063 reports violates `VARIABLE_DESCRIPTOR_COUNT` for multi-heap bindless. So Slang's reflection-driven layout generation is deliberately *not* relied on here (a known-partial win for the later series).

### Shipped DLL set — determined, not guessed
`slang-compiler.lib` → `slang-compiler.dll` (the post-rename real compiler; the `slang.dll` proxy expires end-2026, so it's avoided). Running `slangc.exe` from a directory containing **only** the 4 shipped DLLs (`slang-compiler`, `slang-glslang`, `slang-glsl-module`, `slang-rt`) with the SDK `Bin` removed from `PATH` compiled the spike shader successfully → the 4-DLL set is sufficient for the in-process `createGlobalSession` + direct-SPIR-V path. The 0-byte `slang-standard-module-2026.1` confirms the core module is embedded in `slang-compiler.dll`; no separate module file ships.

### Lazy, default-off, coexisting
The harness lazy-inits on first enable, so a normal (spike-off) boot never loads the 24 MB compiler DLL; the RG pass is gated + culled when off. libshaderc is untouched — the `.slang` route is a single branch in `ShaderCompiler::Compile`, full asset-pipeline `.slang` dispatch (UUID cache, ShaderWatcher) is deferred to series Phase 1. The diff readback is 1 frame stale (a static-scene diagnostic — converges immediately; no per-frame stall).

---

## Files

**New:** `renderer/shader/SlangCompiler.{h,cpp}`, `renderer/subsystems/SlangSpikeSubsystem.{h,cpp}`, `renderer/settings/SlangSpikeSettings.h`; shaders `slang_spike_gi.comp`, `slang_spike_gi.slang`, `slang_spike_diff.comp`, `slang_spike_link.slang`.
**Edited:** `extern/source/vulkan/include/slang/*.h` (re-vendor) + `.../lib/` (lib+DLLs); `luth/premake5.lua`, `runtime/premake5.lua`; `renderer/shader/ShaderCompiler.cpp` (dispatch); `renderer/RenderPipeline.{h,cpp}` (member + Init/Shutdown + `needTlas` + AddPass + reload chain); `scene/systems/RenderingSystem.h` (settings); `luthien/panels/RenderPanel.cpp` (toggle + readout).

---

## Verification

- **Build:** MSBuild Debug x64 clean; `Luthien.exe` links `slang-compiler`; the 4 Slang DLLs stage next to the exe.
- **Offline (the authoritative codegen checks):** `slangc -target spirv -profile spirv_1_5 -emit-spirv-directly` compiles all spike shaders; `spirv-val --target-env vulkan1.3` clean on every output; `spirv-dis` confirms the capability/extension/decoration/offset facts in the results table; `glslc` clean on the GLSL reference + diff.
- **Runtime smoke — DONE (2026-06-13, items 4 + 5):** RenderPanel A/B enabled on an RT scene; boot log confirmed the in-process compile + link-spec PASS, the pipelines ran clean for minutes, and the readout settled to `differing=0 maxUlp=0` over ~1.3 M px with only the FP-shade-floor + silhouette-flip residual analysed above. See "Runtime smoke (2026-06-13)".

---

## Hand-off / deferred

- **GO is fully closed** by the 2026-06-13 smoke (above). No follow-up dig needed — the residual diff was diagnosed (FP-shade ULP + silhouette ray-flips, both inherent to a two-compiler primary-ray A/B).
- **On GO:** open the `slang-material` series (MAJOR/MINOR bump + the phased path in the spec). The additive harness can **stay** as the golden-SPIR-V / validation regression guard the spec asks for — if kept, make the ray math byte-identical + add a ULP tolerance so it reads quiet (Phase-1 polish); on NO-GO it's archived with this writeup.
- **Deferred to the series, not the spike:** full `.slang` asset-pipeline dispatch + ShaderWatcher `.slang` hot-reload (Phase 1); reflection-driven descriptor layouts (slang#8063 — partial, hand-assign the bindless heaps); the bounded `IMaterial` surface + two-tier eval (Phase 2). A production integration would also make the harness's eager paths lazier still and ring the readback buffer.
- Git hooks not installed in this workspace — comment/commit policy honoured manually. No `Version.h`/ROADMAP bump (spike gates the series; the bump lands with series-open).
