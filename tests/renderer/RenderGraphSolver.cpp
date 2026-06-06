// Headless regression tests for the RenderGraph barrier solver (see arch/rendering-pipeline.md).
// Compile() runs with no live VkDevice, so the solve logic is asserted on the CPU with no GPU.
// SetHasSideEffect keeps a test pass alive without faking an attachment (isolates solve from culling).

#include <doctest/doctest.h>

#include "luth/renderer/rendergraph/RenderGraph.h"
#include "luth/memory/LinearAllocator.h"

using namespace Luth;
using namespace Luth::RG;

namespace
{
    struct PassData {};

    TextureDesc ColorDesc(const char* name)
    {
        TextureDesc d; d.name = name; d.width = 16; d.height = 16; d.format = TextureFormat::RGBA16_Float;
        return d;
    }

    // Count barriers in `barriers` matching a before->after transition.
    template <typename Vec>
    int CountTransition(const Vec& barriers, ResourceState before, ResourceState after)
    {
        int n = 0;
        for (const auto& b : barriers)
            if (b.before == before && b.after == after) ++n;
        return n;
    }
}

TEST_CASE("RGSolver: RAW write->read emits ColorAttachment->ShaderResource [rendergraph]")
{
    Memory::LinearAllocator alloc(64 * 1024);
    RenderGraph rg(alloc);

    ResourceHandle tex = rg.RegisterResource(ColorDesc("T"));

    // Producer writes T as a color attachment (color attachment keeps the pass alive).
    rg.AddPass<PassData>("Producer",
        [&](PassData&, RenderPassBuilder& b) { b.Write(tex); },
        [](PassData&, RenderPassContext&) {});

    // Consumer reads T as a sampled image; side-effect keeps it alive without an attachment.
    rg.AddPass<PassData>("Consumer",
        [&](PassData&, RenderPassBuilder& b) { b.SetHasSideEffect(); b.Read(tex); },
        [](PassData&, RenderPassContext&) {});

    rg.Compile();

    const auto& passes = rg.GetPasses();
    REQUIRE(passes.size() == 2);
    CHECK_FALSE(passes[0].culled);
    CHECK_FALSE(passes[1].culled);

    // The read transitions the producer's ColorAttachment output to ShaderResource.
    CHECK(CountTransition(passes[1].preBarriers, ResourceState::ColorAttachment, ResourceState::ShaderResource) == 1);
    // A transient with no external finalState gets no post-barrier.
    CHECK(passes[0].postBarriers.empty());
}

TEST_CASE("RGSolver: WAW same-state write still emits exactly one barrier [rendergraph]")
{
    Memory::LinearAllocator alloc(64 * 1024);
    RenderGraph rg(alloc);

    ResourceHandle tex = rg.RegisterResource(ColorDesc("T"));

    rg.AddPass<PassData>("WriteA",
        [&](PassData&, RenderPassBuilder& b) { b.Write(tex); },
        [](PassData&, RenderPassContext&) {});
    rg.AddPass<PassData>("WriteB",
        [&](PassData&, RenderPassBuilder& b) { b.Write(tex); },
        [](PassData&, RenderPassContext&) {});

    rg.Compile();

    const auto& passes = rg.GetPasses();
    REQUIRE(passes.size() == 2);
    // Consecutive same-state writes still need one execution barrier (Vulkan ordering rule).
    CHECK(CountTransition(passes[1].preBarriers, ResourceState::ColorAttachment, ResourceState::ColorAttachment) == 1);
}

TEST_CASE("RGSolver: cross-queue producer flags crossQueueSrc on the reader [rendergraph]")
{
    Memory::LinearAllocator alloc(64 * 1024);
    RenderGraph rg(alloc);

    ResourceHandle img = rg.RegisterResource(ColorDesc("Img"));

    // Async-compute pass writes the image; a graphics pass then reads it.
    rg.AddComputePass<PassData>("ComputeWrite", QueueFamily::AsyncCompute,
        [&](PassData&, RenderPassBuilder& b) { b.WriteStorageImage(img); },
        [](PassData&, RenderPassContext&) {});
    rg.AddPass<PassData>("GfxRead",
        [&](PassData&, RenderPassBuilder& b) { b.SetHasSideEffect(); b.Read(img); },
        [](PassData&, RenderPassContext&) {});

    rg.Compile();

    const auto& passes = rg.GetPasses();
    REQUIRE(passes.size() == 2);
    REQUIRE(passes[1].preBarriers.size() == 1);
    // The reader runs on graphics, the writer on compute — the pre-barrier src must be cross-queue
    // so Execute substitutes TOP_OF_PIPE (the submit-time semaphore carries the memory dependency).
    CHECK(passes[1].preBarriers[0].crossQueueSrc == true);
}

TEST_CASE("RGSolver: external finalState emits a Present post-barrier on the last writer [rendergraph]")
{
    Memory::LinearAllocator alloc(64 * 1024);
    RenderGraph rg(alloc);

    TextureDesc desc = ColorDesc("Swapchain");
    // Imported external image: starts in ColorAttachment, must end in Present (swapchain handoff).
    ResourceHandle ext = rg.ImportResource(desc, (void*)0x1, (void*)0x2,
                                           ResourceState::ColorAttachment, ResourceState::Present);

    rg.AddPass<PassData>("WriteExt",
        [&](PassData&, RenderPassBuilder& b) { b.Write(ext, VK_ATTACHMENT_LOAD_OP_LOAD); },
        [](PassData&, RenderPassContext&) {});

    rg.Compile();

    const auto& passes = rg.GetPasses();
    REQUIRE(passes.size() == 1);
    CHECK_FALSE(passes[0].culled);
    // finalState drives a post-barrier ColorAttachment -> Present, tagged FINAL.
    int finals = 0;
    for (const auto& b : passes[0].postBarriers)
        if (b.after == ResourceState::Present && b.reason == BarrierReason::Final) ++finals;
    CHECK(finals == 1);
}

TEST_CASE("RGSolver: attachment states carry READ access so loadOp LOAD is covered [rendergraph]")
{
    // Pins the loadOp LOAD fix: the attachment barrier must make the prior write visible to the
    // attachment READ that vkCmdBeginRendering performs for LOAD_OP_LOAD.
    auto [cStage, cAccess] = RenderGraph::GetStateInfo(ResourceState::ColorAttachment);
    CHECK((cAccess & VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT)  != 0);
    CHECK((cAccess & VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT) != 0);

    auto [dStage, dAccess] = RenderGraph::GetStateInfo(ResourceState::DepthStencilAttachment);
    CHECK((dAccess & VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT)  != 0);
    CHECK((dAccess & VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT) != 0);
}
