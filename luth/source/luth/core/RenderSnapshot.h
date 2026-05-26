#pragma once

// Frame-coherent snapshot of game-stage state, consumed by the render stage. Captured at the end
// of the game stage (after Transform / Animation / Material updates) and before
// FrameContext::GameReady signals. Frame N's snapshot is read by frame N+1's render stage, then
// released two frames later when the FrameContext ring slot resets.
//
// Lives in core/ alongside FrameData so renderer modules depend on core, not the reverse.
// Implementation in luth/core/RenderSnapshot.cpp.

#include "luth/core/types/LuthMath.h"
#include "luth/core/types/LuthTypes.h"
#include "luth/core/UUID.h"
#include "luth/renderer/lighting/LightTypes.h"  // ShadowingMode

#include <span>

namespace Luth
{
    namespace Memory { class LinearAllocator; }
    class Scene;

    // ── Per-draw snapshot ──
    // One row per (WorldTransform + MeshRenderer) entity. Animation parent chain is pre-resolved
    // here so the render stage never walks the registry to find a bone offset.

    struct MeshDrawSnapshot
    {
        Mat4 worldMatrix{ 1.0f };
        UUID modelUUID;
        UUID materialUUID;
        u32  meshIndex   = 0;
        u32  boneOffset  = 0;        // Pre-resolved from Animation / Parent chain
        bool isSkinned   = false;
        u32  entity      = 0;        // entt::entity underlying value — cast at the use site
    };

    // ── Lighting snapshots ──

    struct DirectionalLightSnapshot
    {
        bool present = false;
        Vec3 direction{ 0.0f, -1.0f, 0.0f };
        Vec3 color{ 1.0f };
        f32  intensity = 1.0f;
        bool castShadows = false;
        Vec4 shadowBias{ 0.0f };
        Vec4 shadowNormalBias{ 0.0f };
        f32  splitLambda = 0.5f;
        f32  shadowDistance = 100.0f;
        bool stabilizeCascades = false;
        f32  cascadeBlendWidth = 0.0f;
        bool debugVisualizeCascades = false;

        // RT-shadow toggle + bias (consumed when shadowing == RtShadows).
        ShadowingMode shadowing       = ShadowingMode::RtShadows;
        f32           rtOriginEpsilon = 0.001f;
        f32           rtNormalEpsilon = 0.05f;
    };

    struct PointLightSnapshot
    {
        Vec3 position{ 0.0f };
        Vec3 color{ 1.0f };
        f32  intensity = 1.0f;
        f32  range = 10.0f;
    };

    // ── Volumetric fog snapshots ──
    // worldMatrix bakes the entity's WorldTransform * fog local-offset/rotation so the injection
    // shader gets a single volume-to-world transform per row. extentsOrRadius carries Box
    // halfExtents (xyz) or Sphere radius (x, others ignored) per `type`.

    struct FogVolumeSnapshot
    {
        Mat4 worldMatrix{ 1.0f };
        u32  type = 0;                  // 0 = Box, 1 = Sphere — matches FogVolume::Type
        Vec3 extentsOrRadius{ 1.0f };
        Vec3 color{ 1.0f };
        f32  density = 0.1f;
        f32  falloffStart = 0.0f;
        f32  falloffEnd = 1.0f;
        bool affectsAmbient = true;
    };

    // ── RenderSnapshot ──
    // POD aggregate. Spans point into a per-frame LinearAllocator (FrameContext::LogicMemory),
    // which resets two iterations later — backing memory survives long enough for one render pass.

    struct RenderSnapshot
    {
        std::span<const MeshDrawSnapshot>   meshes;
        std::span<const PointLightSnapshot> pointLights;
        std::span<const FogVolumeSnapshot>  fogVolumes;
        DirectionalLightSnapshot            directionalLight;
        std::span<const char* const>        tagsByEntity;   // debug-only; empty when frame-debugger not capturing

        void Clear()
        {
            meshes = {};
            pointLights = {};
            fogVolumes = {};
            directionalLight = {};
            tagsByEntity = {};
        }
    };

    // ── Capture API ──
    // Walks the registry once, allocates the spans from `mem`, fills `out`.

    void CaptureSnapshot(Scene& scene, Memory::LinearAllocator& mem, RenderSnapshot& out);
}
