#pragma once

#include "luth/core/HarnessSpec.h"
#include "luth/core/types/LuthMath.h"

namespace Luth
{
    struct CameraParams;

    // Offscreen capture driver for LuthShot.
    //
    // Drives the PRIMARY view rather than queueing a second one. RenderingSystem::Update runs the
    // per-frame lighting + CSM cascade fit once, against the primary view's camera, so a queued view
    // with an unrelated camera renders against shared state fit to somebody else's frustum. With no
    // editor there is no competing viewport, so the primary view is simply ours.
    class ScreenshotHarness
    {
    public:
        explicit ScreenshotHarness(const HarnessSpec& spec) : m_Spec(spec) {}

        // Call before RenderingSystem::SetCameraParams. Returns false while the scene is still streaming,
        // in which case the caller should leave the camera alone and try again next frame.
        bool ResolveCamera(CameraParams& out);

        // Call post-render on the main thread, alongside the PickingSystem readback: both consume render
        // targets the render stage just wrote. Returns true once the capture is done and the app should stop.
        bool CaptureIfReady();

        bool Failed() const { return m_Failed; }

    private:
        bool BuildCamera(CameraParams& out);
        bool AutoFrame(CameraParams& out);
        bool WritePng();

        // Models stream in on worker fibers, so the scene is empty for the first frames and its bounds
        // keep growing after that. Giving up on frame 0 would capture nothing; instead the warmup counter
        // restarts whenever the bounds move, and only a scene that never resolves is an error.
        static constexpr u32 k_MaxWaitFrames = 600;

        HarnessSpec m_Spec;
        AABB m_LastBounds;
        bool m_Failed     = false;
        bool m_Resolved   = false;
        u32  m_WaitFrames = 0;
        u32  m_Rendered   = 0;
    };
}
