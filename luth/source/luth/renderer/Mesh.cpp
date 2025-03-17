#include "luthpch.h"

#include "luth/renderer/Mesh.h"
#include "luth/renderer/openGL/GLMesh.h"
#include "luth/renderer/vulkan/VKMesh.h"
#include "luth/renderer/Renderer.h"
#include "luth/renderer/RendererAPI.h"

namespace Luth
{
    std::shared_ptr<Mesh> Mesh::Create(const std::shared_ptr<VertexBuffer>& vb,
        const std::shared_ptr<IndexBuffer>& ib,
        const std::shared_ptr<Texture>& texture)
    {
        switch (Renderer::GetAPI())
        {
            case RendererAPI::API::OpenGL:
            {
                auto glVB = std::dynamic_pointer_cast<GLVertexBuffer>(vb);
                auto glIB = std::dynamic_pointer_cast<GLIndexBuffer>(ib);
                auto glTex = std::dynamic_pointer_cast<GLTexture>(texture);

                LH_CORE_ASSERT(glVB, "VertexBuffer is not a GLVertexBuffer!");
                LH_CORE_ASSERT(glIB, "IndexBuffer is not a GLIndexBuffer!");
                LH_CORE_ASSERT(glTex, "Texture is not a GLTexture!");

                return std::make_shared<GLMesh>(glVB, glIB, glTex);
            }

            case RendererAPI::API::Vulkan:
                //return std::make_shared<VKMesh>(vb, ib, texture);
            default:
                LH_CORE_ASSERT(false, "Unknown renderer API!");
                return nullptr;
        }
    }
}
