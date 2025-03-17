#pragma once

#include "luth/core/Log.h"
#include "luth/renderer/Mesh.h"
#include "luth/renderer/openGL/GLBuffer.h"
#include "luth/resources/GLTexture.h"

#include <glad/glad.h>

namespace Luth
{
    class GLMesh : public Mesh
    {
    public:
        GLMesh(const std::shared_ptr<GLVertexBuffer>& vertexBuffer,
            const std::shared_ptr<GLIndexBuffer>& indexBuffer,
            const std::shared_ptr<GLTexture>& texture)
            : m_VertexBuffer(vertexBuffer),
            m_IndexBuffer(indexBuffer),
            m_Texture(texture)
        {
            CreateVAO();
        }

        void Bind() const override {
            glBindVertexArray(m_VAO);
        }

        void Draw() const override {
            if (m_Texture) m_Texture->Bind(0);
            glBindVertexArray(m_VAO);
            if (m_IndexBuffer) {
                glDrawElements(GL_TRIANGLES, m_IndexBuffer->GetCount(), GL_UNSIGNED_INT, nullptr);
            }
            else {
                //glDrawArrays(GL_TRIANGLES, 0, m_VertexBuffer->GetVertexCount());
            }
        }

    private:
        void CreateVAO() {
            glGenVertexArrays(1, &m_VAO);
            glBindVertexArray(m_VAO);

            // Bind vertex buffer
            m_VertexBuffer->Bind();

            // Set vertex attributes from layout
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
                    (const void*)(intptr_t)element.Offset
                );
                index++;
            }

            // Bind index buffer
            m_IndexBuffer->Bind();

            glBindVertexArray(0);
        }

        static GLenum ShaderDataTypeToGLType(ShaderDataType type) {
            switch (type) {
            case ShaderDataType::Float:    return GL_FLOAT;
            case ShaderDataType::Float2:   return GL_FLOAT;
            case ShaderDataType::Float3:   return GL_FLOAT;
            case ShaderDataType::Float4:   return GL_FLOAT;
            case ShaderDataType::Int:     return GL_INT;
            case ShaderDataType::Int2:    return GL_INT;
            case ShaderDataType::Int3:    return GL_INT;
            case ShaderDataType::Int4:    return GL_INT;
            case ShaderDataType::Mat3:    return GL_FLOAT;
            case ShaderDataType::Mat4:    return GL_FLOAT;
            case ShaderDataType::Bool:    return GL_BOOL;
            default:
                LH_CORE_ASSERT(false, "Unknown ShaderDataType!");
                return 0;
            }
        }

        GLuint m_VAO;
        std::shared_ptr<GLVertexBuffer> m_VertexBuffer;
        std::shared_ptr<GLIndexBuffer> m_IndexBuffer;
        std::shared_ptr<GLTexture> m_Texture;
    };
}
