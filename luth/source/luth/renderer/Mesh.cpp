#include "luthpch.h"

#include "luth/renderer/Mesh.h"
#include "luth/renderer/Renderer.h"
#include "luth/renderer/RendererAPI.h"

namespace Luth
{
    std::shared_ptr<Mesh> Mesh::Create(
        const std::shared_ptr<VertexBuffer>& vb,
        const std::shared_ptr<IndexBuffer>& ib)
    {
        return std::make_shared<Mesh>(vb, ib);
    }
}
