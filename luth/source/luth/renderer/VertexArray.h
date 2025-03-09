#pragma once

#include "luth/renderer/Buffer.h"

#include <memory>
#include <vector>

namespace Luth
{
    class VertexArray
    {
    public:
        virtual ~VertexArray() = default;

        virtual void Bind() const = 0;
        virtual void Unbind() const = 0;

        virtual void AddVertexBuffer(const std::shared_ptr<VertexBuffer>& vb) = 0;
        virtual void SetIndexBuffer(const std::shared_ptr<IndexBuffer>& ib) = 0;

        virtual const std::vector<std::shared_ptr<VertexBuffer>>& GetVertexBuffers() const = 0;
        virtual const std::shared_ptr<IndexBuffer>& GetIndexBuffer() const = 0;

        static std::unique_ptr<VertexArray> Create();
    };
}
