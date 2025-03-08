#pragma once

#include "luth/renderer/IndexBuffer.h"

namespace Luth
{
    class GLIndexBuffer : public IndexBuffer
    {
    public:
        GLIndexBuffer(const uint32_t* indices, uint32_t count);
        ~GLIndexBuffer();

        void Bind() const override;
        void Unbind() const override;
        uint32_t GetCount() const override { return m_Count; }

    private:
        uint32_t m_RendererID;
        uint32_t m_Count;
    };
}
