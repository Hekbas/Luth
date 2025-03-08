#include "luthpch.h"
#include "luth/renderer/openGL/GLIndexBuffer.h"

#include <glad/glad.h>

namespace Luth
{
    GLIndexBuffer::GLIndexBuffer(const uint32_t* indices, uint32_t count)
        : m_Count(count) {
        glGenBuffers(1, &m_RendererID);
        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, m_RendererID);
        glBufferData(GL_ELEMENT_ARRAY_BUFFER, count * sizeof(uint32_t), indices, GL_STATIC_DRAW);
    }

    GLIndexBuffer::~GLIndexBuffer() {
        glDeleteBuffers(1, &m_RendererID);
    }

    void GLIndexBuffer::Bind() const {
        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, m_RendererID);
    }

    void GLIndexBuffer::Unbind() const {
        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);
    }
}
