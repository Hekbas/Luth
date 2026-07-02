#include "luthpch.h"

#include "luth/renderer/resources/Mesh.h"
#include "luth/renderer/backend/vulkan/VulkanAccelerationStructure.h"
#include "luth/renderer/Renderer.h"
#include "luth/renderer/RenderBackend.h"

namespace Luth
{
    // Out-of-line for m_Blas (shared_ptr<VKAccelerationStructure> with incomplete type at header).
    Mesh::~Mesh() = default;

    std::shared_ptr<Mesh> Mesh::Create(
        const std::shared_ptr<VertexBuffer>& vb,
        const std::shared_ptr<IndexBuffer>& ib,
        uint32_t vertexCount,
        bool isSkinned)
    {
        // BLAS construction happens at Model::ProcessMeshData where the CPU SkinnedVertex source
        // data is available; keeps static + skinned paths uniform at one call site.
        return std::make_shared<Mesh>(vb, ib, vertexCount, isSkinned);
    }
}
