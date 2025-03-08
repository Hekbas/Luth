#pragma once

#include <memory>

namespace Luth
{
    class IndexBuffer
    {
    public:
        virtual ~IndexBuffer() = default;

        virtual void Bind() const = 0;
        virtual void Unbind() const = 0;
        virtual uint32_t GetCount() const = 0;

        static std::unique_ptr<IndexBuffer> Create(const uint32_t* indices, uint32_t count);
    };
}
