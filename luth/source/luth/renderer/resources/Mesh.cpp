#include "luthpch.h"

#include "luth/renderer/resources/Mesh.h"
#include "luth/renderer/backend/vulkan/VulkanAccelerationStructure.h"
#include "luth/renderer/Renderer.h"
#include "luth/renderer/RenderBackend.h"

namespace Luth
{
    Mesh::~Mesh() = default;

    std::shared_ptr<Mesh> Mesh::Create(
        const std::shared_ptr<VertexBuffer>& vb,
        const std::shared_ptr<IndexBuffer>& ib,
        uint32_t vertexCount,
        bool isSkinned)
    {
        auto mesh = std::make_shared<Mesh>(vb, ib, vertexCount, isSkinned);
        // Skinned BLAS uses ALLOW_UPDATE + per-frame compute-skin refit on a separate path —
        // skipped here so m_Blas stays null until that path fills it. Static gets built now.
        if (!isSkinned)
            mesh->SetBlas(VKAccelerationStructure::CreateStaticBLAS(*mesh));
        return mesh;
    }
}
