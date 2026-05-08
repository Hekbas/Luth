// Standalone harness for validating LuthJobSystemForJolt against a real JPH::PhysicsSystem workload.
// Drops 1000 dynamic spheres onto a static floor and steps the simulation through the adapter; the point
// isn't physical correctness — it's putting enough parallel work through the adapter for Tracy's fiber view
// to show worker fibers yielding through Luth's V5 wait path under contention.
//
// Usage:
//   jobsys_proof.exe                # 600 frames after ENTER prompt
//   jobsys_proof.exe --frames 1200  # custom frame count
//   jobsys_proof.exe --no-prompt    # don't wait for ENTER (CI mode)

#include <Jolt/Jolt.h>

#include <Jolt/RegisterTypes.h>
#include <Jolt/Core/Factory.h>
#include <Jolt/Core/TempAllocator.h>
#include <Jolt/Physics/PhysicsSettings.h>
#include <Jolt/Physics/PhysicsSystem.h>
#include <Jolt/Physics/Collision/Shape/BoxShape.h>
#include <Jolt/Physics/Collision/Shape/SphereShape.h>
#include <Jolt/Physics/Body/BodyCreationSettings.h>

#include "luth/core/diagnostics/Log.h"
#include "luth/jobs/JobSystem.h"
#include "luth/physics/LuthJobSystemForJolt.h"

#include <chrono>
#include <cstring>
#include <iostream>
#include <string>
#include <vector>

JPH_SUPPRESS_WARNINGS

using namespace JPH;
using namespace JPH::literals;

// ── Layer setup ─────────────────────────────────────────────────────────

namespace Layers
{
    static constexpr ObjectLayer NON_MOVING = 0;
    static constexpr ObjectLayer MOVING     = 1;
    static constexpr ObjectLayer NUM_LAYERS = 2;
}

namespace BroadPhaseLayers
{
    static constexpr BroadPhaseLayer NON_MOVING(0);
    static constexpr BroadPhaseLayer MOVING(1);
    static constexpr uint            NUM_LAYERS(2);
}

class ObjectLayerPairFilterImpl final : public ObjectLayerPairFilter
{
public:
    bool ShouldCollide(ObjectLayer a, ObjectLayer b) const override
    {
        if (a == Layers::NON_MOVING) return b == Layers::MOVING;
        if (a == Layers::MOVING)     return true;
        return false;
    }
};

class BPLayerInterfaceImpl final : public BroadPhaseLayerInterface
{
public:
    BPLayerInterfaceImpl()
    {
        m_Map[Layers::NON_MOVING] = BroadPhaseLayers::NON_MOVING;
        m_Map[Layers::MOVING]     = BroadPhaseLayers::MOVING;
    }
    uint            GetNumBroadPhaseLayers() const override          { return BroadPhaseLayers::NUM_LAYERS; }
    BroadPhaseLayer GetBroadPhaseLayer(ObjectLayer l) const override { return m_Map[l]; }
#if defined(JPH_PROFILE_ENABLED) || defined(JPH_EXTERNAL_PROFILE)
    const char*     GetBroadPhaseLayerName(BroadPhaseLayer) const override { return "BP"; }
#endif
private:
    BroadPhaseLayer m_Map[Layers::NUM_LAYERS];
};

class ObjectVsBroadPhaseLayerFilterImpl final : public ObjectVsBroadPhaseLayerFilter
{
public:
    bool ShouldCollide(ObjectLayer a, BroadPhaseLayer b) const override
    {
        if (a == Layers::NON_MOVING) return b == BroadPhaseLayers::MOVING;
        if (a == Layers::MOVING)     return true;
        return false;
    }
};

// ── Argument parsing ────────────────────────────────────────────────────

struct Args
{
    int  frames    = 600;   // 10s at 60Hz — enough for Tracy to capture meaningful patterns
    bool prompt    = true;
    int  numBodies = 1000;
};

static Args ParseArgs(int argc, char** argv)
{
    Args a{};
    for (int i = 1; i < argc; ++i)
    {
        if (std::strcmp(argv[i], "--frames") == 0 && i + 1 < argc)
            a.frames = std::stoi(argv[++i]);
        else if (std::strcmp(argv[i], "--bodies") == 0 && i + 1 < argc)
            a.numBodies = std::stoi(argv[++i]);
        else if (std::strcmp(argv[i], "--no-prompt") == 0)
            a.prompt = false;
    }
    return a;
}

// ── main ───────────────────────────────────────────────────────────────

int main(int argc, char** argv)
{
    using namespace std::chrono;

    const Args args = ParseArgs(argc, argv);

    std::cout << "Luth JobSystem + Jolt adapter proof\n";
    std::cout << "  frames:    " << args.frames    << "\n";
    std::cout << "  bodies:    " << args.numBodies << "\n";

    // Logger must come up before anything that calls LH_CORE_* macros — the JobSystem and
    // TaggedPageAllocator both log during Init.
    Luth::Log::Init();

    std::cout << "[1] Init Luth JobSystem...\n" << std::flush;
    Luth::JobSystem::Init();
    std::cout << "[2] JobSystem stats: workers=" << Luth::JobSystem::GetStats().ThreadCount << "\n" << std::flush;

    std::cout << "[3] Init Jolt...\n" << std::flush;
    RegisterDefaultAllocator();
    Factory::sInstance = new Factory();
    RegisterTypes();
    std::cout << "[4] Jolt registered\n" << std::flush;

    {
        std::cout << "[5] TempAllocator...\n" << std::flush;
        TempAllocatorImpl tempAllocator(32 * 1024 * 1024);

        std::cout << "[6] Construct adapter...\n" << std::flush;
        Luth::Physics::LuthJobSystemForJolt jobSystem(2048, 8);
        std::cout << "[7] Adapter MaxConcurrency=" << jobSystem.GetMaxConcurrency() << "\n" << std::flush;

        const uint cMaxBodies            = (uint)args.numBodies + 16;
        const uint cNumBodyMutexes       = 0;     // 0 = default
        const uint cMaxBodyPairs         = 65536;
        const uint cMaxContactConstraints = 16384;

        BPLayerInterfaceImpl              bpLayers;
        ObjectVsBroadPhaseLayerFilterImpl ovbpFilter;
        ObjectLayerPairFilterImpl         olpFilter;

        PhysicsSystem physicsSystem;
        physicsSystem.Init(cMaxBodies, cNumBodyMutexes, cMaxBodyPairs, cMaxContactConstraints,
                           bpLayers, ovbpFilter, olpFilter);

        BodyInterface& bi = physicsSystem.GetBodyInterface();

        // Static floor: 200×2×200 box centred at the origin, top surface at y=0.
        BoxShapeSettings floorSettings(Vec3(100.0f, 1.0f, 100.0f));
        floorSettings.SetEmbedded();
        ShapeRefC floorShape = floorSettings.Create().Get();

        BodyCreationSettings floorBcs(floorShape, RVec3(0, -1, 0), Quat::sIdentity(),
                                      EMotionType::Static, Layers::NON_MOVING);
        BodyID floorId = bi.CreateAndAddBody(floorBcs, EActivation::DontActivate);

        // Dynamic spheres on a 10×10×N grid above the floor — guaranteed contact workload as the column
        // collapses onto itself.
        SphereShapeSettings sphereSettings(0.5f);
        sphereSettings.SetEmbedded();
        ShapeRefC sphereShape = sphereSettings.Create().Get();

        std::vector<BodyID> bodies;
        bodies.reserve(args.numBodies);
        const int sideX = 10;
        const int sideZ = 10;
        const float spacing = 1.2f;
        for (int i = 0; i < args.numBodies; ++i)
        {
            const int gx = i % sideX;
            const int gz = (i / sideX) % sideZ;
            const int gy = i / (sideX * sideZ);
            const float x = (gx - sideX * 0.5f) * spacing;
            const float z = (gz - sideZ * 0.5f) * spacing;
            const float y = 5.0f + gy * spacing;
            BodyCreationSettings bcs(sphereShape, RVec3(x, y, z), Quat::sIdentity(),
                                     EMotionType::Dynamic, Layers::MOVING);
            bodies.push_back(bi.CreateAndAddBody(bcs, EActivation::Activate));
        }

        std::cout << "[10] OptimizeBroadPhase...\n" << std::flush;
        physicsSystem.OptimizeBroadPhase();
        std::cout << "[11] Ready to step\n" << std::flush;

        if (args.prompt)
        {
            std::cout << "\nAttach Tracy now, then press ENTER to start stepping...\n";
            std::cin.get();
        }

        const float dt              = 1.0f / 60.0f;
        const int   collisionSteps  = 1;

        std::cout << "Stepping " << args.frames << " frames\n" << std::flush;
        const auto t0 = high_resolution_clock::now();
        for (int frame = 0; frame < args.frames; ++frame)
        {
            physicsSystem.Update(dt, collisionSteps, &tempAllocator, &jobSystem);
            // Flush every 30 frames so progress is visible when stdout is piped.
            if ((frame + 1) % 30 == 0)
            {
                const auto tn = high_resolution_clock::now();
                const double elapsedMs = duration<double, std::milli>(tn - t0).count();
                std::cout << "  frame " << (frame + 1) << "/" << args.frames
                          << "  elapsed=" << elapsedMs << " ms\n" << std::flush;
            }
        }
        const auto t1 = high_resolution_clock::now();
        const double totalMs = duration<double, std::milli>(t1 - t0).count();

        std::cout << "Total: " << totalMs << " ms (" << (totalMs / args.frames)
                  << " ms/step)\n" << std::flush;

        // Sanity: pick a body, print its final Y. Should be near the floor (~0.5).
        const RVec3 sample = bi.GetCenterOfMassPosition(bodies.front());
        std::cout << "First body Y: " << sample.GetY() << "\n";

        // Cleanup bodies.
        for (BodyID id : bodies) bi.RemoveBody(id);
        for (BodyID id : bodies) bi.DestroyBody(id);
        bi.RemoveBody(floorId);
        bi.DestroyBody(floorId);
    }

    UnregisterTypes();
    delete Factory::sInstance;
    Factory::sInstance = nullptr;

    Luth::JobSystem::Shutdown();

    std::cout << "OK\n";
    return 0;
}
