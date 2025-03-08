#pragma once

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

        static std::unique_ptr<VertexBuffer> Create(uint32_t size);
        static std::unique_ptr<VertexBuffer> Create(const void* data, uint32_t size);
    };
}
