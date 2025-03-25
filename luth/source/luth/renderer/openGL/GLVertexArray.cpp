#include "luthpch.h"
#include "luth/renderer/openGL/GLVertexArray.h"
#include "luth/renderer/openGL/GLBuffer.h"

#include <glad/glad.h>

namespace Luth
{
    GLVertexArray::GLVertexArray() {
        glGenVertexArrays(1, &m_VertexArrayID);
    }

    GLVertexArray::~GLVertexArray() {
        glDeleteVertexArrays(1, &m_VertexArrayID);
    }

    void GLVertexArray::Bind() const {
        glBindVertexArray(m_VertexArrayID);
    }

    void GLVertexArray::Unbind() const {
        glBindVertexArray(0);
    }

    void GLVertexArray::AddVertexBuffer(const std::shared_ptr<VertexBuffer>& vb)
    {
        glBindVertexArray(m_VertexArrayID);
        vb->Bind();

        const auto& layout = vb->GetLayout();
        uint32_t index = 0;
        for (const auto& element : layout.GetElements()) {
            glEnableVertexAttribArray(index);
            glVertexAttribPointer(
                index,
                element.GetComponentCount(),
                GL_FLOAT,
                element.Normalized ? GL_TRUE : GL_FALSE,
                layout.GetStride(),
                (const void*)element.Offset
            );
            index++;
        }
        m_VertexBuffers.push_back(vb);
    }

    void GLVertexArray::SetIndexBuffer(const std::shared_ptr<IndexBuffer>& ib)
    {
        glBindVertexArray(m_VertexArrayID);
        ib->Bind();
        m_IndexBuffer = ib;
    }
}
