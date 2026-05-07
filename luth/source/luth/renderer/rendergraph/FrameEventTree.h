#pragma once

#include "luth/core/types/LuthTypes.h"

#include <string>
#include <vector>

namespace Luth::RG
{
    struct CapturedFrame;

    // Hierarchical event tree shown in the Frame Debugger panel. Built once at capture finalize
    // from CapturedFrame::passes + drawCalls, then traversed recursively by the panel's
    // DrawEventNode. Selection is stored on the panel as (passIndex, drawIndex) and resolved
    // through the tree by walking children, which keeps the model simple and serialization-free.
    //
    // Group routing uses an explicit prefix registry inside BuildEventTree (NOT a generic
    // split-on-dot, which would be brittle when a pass name happens to contain a period).
    // Today's groups: "Frustum Culling" and "Shadows".
    enum class EventNodeKind : u8 { Group, Pass, Draw, Cascade };

    struct EventNode
    {
        EventNodeKind kind = EventNodeKind::Group;
        std::string  label;

        u32  passIndex          = UINT32_MAX;  // index into CapturedFrame::passes
        u32  drawIndex          = UINT32_MAX;  // index into CapturedFrame::drawCalls (Draw kind only)
        int  archivedImageIndex = -1;           // index into CapturedFrame::archivedImages
        int  archiveLayer       = -1;           // -1 = whole image, else 2D-array slice (Cascade kind)
        float gpuTimeMs         = -1.0f;

        // Last leaf draw index reachable in this subtree (Unity-style scrub
        // semantics — clicking a Group/Pass/Cascade snaps the draw slider to
        // this value). UINT32_MAX = no Draw descendants. For Draw nodes this
        // equals drawIndex.
        u32  lastDrawIndex      = UINT32_MAX;

        std::vector<EventNode> children;
    };

    // Build the hierarchical tree from a captured frame's flat metadata.
    // Pure function — does not touch GPU state. Safe to call from FinalizeCapture
    // after all CapturedPass / CapturedDrawCall records are populated.
    EventNode BuildEventTree(const CapturedFrame& frame);
}
