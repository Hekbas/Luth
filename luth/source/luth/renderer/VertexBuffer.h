#pragma once

#include "luth/renderer/BufferLayout.h"

#include <memory>

namespace Luth
{
    class VertexBuffer
    {
    public:
        virtual ~VertexBuffer() = default;

        virtual void Bind() const = 0;
        virtual void Unbind() const = 0;
        virtual void SetData(const void* data, uint32_t size) = 0;
        virtual const BufferLayout& GetLayout() const = 0;
        virtual void SetLayout(const BufferLayout& layout) = 0;

        static std::unique_ptr<VertexBuffer> Create(uint32_t size);
        static std::unique_ptr<VertexBuffer> Create(const void* data, uint32_t size);
    };
}
