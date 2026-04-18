#include "luthpch.h"

#include "luth/renderer/resources/Mesh.h"
#include "luth/renderer/Renderer.h"
#include "luth/renderer/RenderBackend.h"

namespace Luth
{
    std::shared_ptr<Mesh> Mesh::Create(
        const std::shared_ptr<VertexBuffer>& vb,
        const std::shared_ptr<IndexBuffer>& ib)
    {
        return std::make_shared<Mesh>(vb, ib);
    }
}
