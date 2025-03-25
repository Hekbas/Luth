#include "luthpch.h"
#include "luth/renderer/openGL/GLBuffer.h"

#include <glad/glad.h>

namespace Luth
{
    // Vertex Buffer
    // ------------------------
    GLVertexBuffer::GLVertexBuffer(uint32_t size)
    {
        glCreateBuffers(1, &m_BufferID);
        glBindBuffer(GL_ARRAY_BUFFER, m_BufferID);
        glBufferData(GL_ARRAY_BUFFER, size, nullptr, GL_DYNAMIC_DRAW);
    }

    GLVertexBuffer::GLVertexBuffer(const void* data, uint32_t size)
    {
        glCreateBuffers(1, &m_BufferID);
        glBindBuffer(GL_ARRAY_BUFFER, m_BufferID);
        glBufferData(GL_ARRAY_BUFFER, size, data, GL_STATIC_DRAW);
    }

    GLVertexBuffer::~GLVertexBuffer()
    {
        glDeleteBuffers(1, &m_BufferID);
    }

    void GLVertexBuffer::Bind() const
    {
        glBindBuffer(GL_ARRAY_BUFFER, m_BufferID);
    }

    void GLVertexBuffer::Unbind() const
    {
        glBindBuffer(GL_ARRAY_BUFFER, 0);
    }

    void GLVertexBuffer::SetData(const void* data, uint32_t size)
    {
        glNamedBufferSubData(m_BufferID, 0, size, data);
    }

    // Index Buffer
    // ------------------------
    GLIndexBuffer::GLIndexBuffer(const uint32_t* indices, uint32_t count)
        : m_Count(count) {
        glGenBuffers(1, &m_BufferID);
        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, m_BufferID);
        glBufferData(GL_ELEMENT_ARRAY_BUFFER, count * sizeof(uint32_t), indices, GL_STATIC_DRAW);
    }

    GLIndexBuffer::~GLIndexBuffer() {
        glDeleteBuffers(1, &m_BufferID);
    }

    void GLIndexBuffer::Bind() const {
        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, m_BufferID);
    }

    void GLIndexBuffer::Unbind() const {
        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);
    }
}
