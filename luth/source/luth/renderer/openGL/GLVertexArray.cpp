#include "luthpch.h"
#include "luth/renderer/openGL/GLVertexArray.h"
#include "luth/renderer/openGL/GLVertexBuffer.h"

#include <glad/glad.h>

namespace Luth
{
    GLVertexArray::GLVertexArray() {
        glGenVertexArrays(1, &m_RendererID);
    }

    GLVertexArray::~GLVertexArray() {
        glDeleteVertexArrays(1, &m_RendererID);
    }

    void GLVertexArray::Bind() const {
        glBindVertexArray(m_RendererID);
    }

    void GLVertexArray::Unbind() const {
        glBindVertexArray(0);
    }

    void GLVertexArray::AddVertexBuffer(const std::shared_ptr<VertexBuffer>& vb)
    {
        glBindVertexArray(m_RendererID);
        vb->Bind();

        // Vertex buffer layout implementation needed here
        // This would iterate through buffer elements and set glVertexAttribPointer

        m_VertexBuffers.push_back(vb);
    }

    void GLVertexArray::SetIndexBuffer(const std::shared_ptr<IndexBuffer>& ib)
    {
        glBindVertexArray(m_RendererID);
        ib->Bind();
        m_IndexBuffer = ib;
    }
}
