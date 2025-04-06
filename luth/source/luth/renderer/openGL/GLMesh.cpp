#include "luthpch.h"
#include "luth/renderer/openGL/GLMesh.h"
#include "luth/resources/libraries/TextureCache.h"

namespace Luth
{
    GLMesh::GLMesh(const std::shared_ptr<GLVertexBuffer>& vertexBuffer,
        const std::shared_ptr<GLIndexBuffer>& indexBuffer)
        : m_VertexBuffer(vertexBuffer), m_IndexBuffer(indexBuffer)
    {
        CreateVAO();
    }

    void GLMesh::Bind() const
    {
        glBindVertexArray(m_VAO);
    }

    void GLMesh::Draw() const
    {
        glBindVertexArray(m_VAO);
        if (m_IndexBuffer) {
            //glLineWidth(0.1f);
            //glPolygonMode(GL_FRONT_AND_BACK, GL_LINE);
            glDrawElements(GL_TRIANGLES, m_IndexBuffer->GetCount(), GL_UNSIGNED_INT, nullptr);
        }
        else {
            // glDrawArrays(GL_TRIANGLES, 0, m_VertexBuffer->GetVertexCount());
        }
    }

    void GLMesh::CreateVAO()
    {
        glGenVertexArrays(1, &m_VAO);
        glBindVertexArray(m_VAO);

        m_VertexBuffer->Bind();

        const auto& layout = m_VertexBuffer->GetLayout();
        uint32_t index = 0;
        uint32_t stride = layout.GetStride();

        for (const auto& element : layout.GetElements()) {
            glEnableVertexAttribArray(index);
            glVertexAttribPointer(
                index,
                element.GetComponentCount(),
                ShaderDataTypeToGLType(element.Type),
                element.Normalized ? GL_TRUE : GL_FALSE,
                stride,
                (const void*)(intptr_t)element.Offset);
            index++;
        }

        m_IndexBuffer->Bind();

        glBindVertexArray(0);
    }

    GLenum GLMesh::ShaderDataTypeToGLType(ShaderDataType type)
    {
        switch (type) {
            case ShaderDataType::Float:    return GL_FLOAT;
            case ShaderDataType::Float2:   return GL_FLOAT;
            case ShaderDataType::Float3:   return GL_FLOAT;
            case ShaderDataType::Float4:   return GL_FLOAT;
            case ShaderDataType::Int:      return GL_INT;
            case ShaderDataType::Int2:     return GL_INT;
            case ShaderDataType::Int3:     return GL_INT;
            case ShaderDataType::Int4:     return GL_INT;
            case ShaderDataType::Mat3:     return GL_FLOAT;
            case ShaderDataType::Mat4:     return GL_FLOAT;
            case ShaderDataType::Bool:     return GL_BOOL;
            default:
                LH_CORE_ASSERT(false, "Unknown ShaderDataType!");
                return 0;
        }
    }
}
