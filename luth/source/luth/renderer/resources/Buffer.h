#pragma once

#include "luth/core/Math.h"

#include <vector>
#include <string>
#include <initializer_list>
#include <memory>
#include <vulkan/vulkan.h>

namespace Luth
{
    enum class ShaderDataType
    {
        None = 0,
        Bool,
        Int, Int2, Int3, Int4,
        Float, Float2, Float3, Float4,
        Mat3, Mat4
    };

    struct BufferElement
    {
        std::string Name;
        ShaderDataType Type;
        uint32_t Size;
        uint32_t Offset;
        bool Normalized;

        BufferElement() = default;
        BufferElement(ShaderDataType type, const std::string& name, bool normalized = false);

        uint32_t GetComponentCount() const;
    };

    class BufferLayout
    {
    public:
        BufferLayout() = default;
        BufferLayout(std::initializer_list<BufferElement> elements);

        const std::vector<BufferElement>& GetElements() const { return m_Elements; }
        uint32_t GetStride() const { return m_Stride; }

        // For Vulkan compatibility
        std::vector<VkVertexInputBindingDescription> GetBindingDescriptions() const;
        std::vector<VkVertexInputAttributeDescription> GetAttributeDescriptions() const;

    private:
        void CalculateOffsetsAndStride();

        std::vector<BufferElement> m_Elements;
        uint32_t m_Stride = 0;
    };

    class VertexBuffer
    {
    public:
        virtual ~VertexBuffer() = default;

        virtual void Bind() const = 0;
        virtual void Unbind() const = 0;
        virtual void SetData(const void* data, uint32_t size) = 0;
        virtual const BufferLayout& GetLayout() const = 0;
        virtual void SetLayout(const BufferLayout& layout) = 0;

        static std::shared_ptr<VertexBuffer> Create(uint32_t size);
        static std::shared_ptr<VertexBuffer> Create(const void* data, uint32_t size);
    };

    class IndexBuffer
    {
    public:
        virtual ~IndexBuffer() = default;

        virtual void Bind() const = 0;
        virtual void Unbind() const = 0;
        virtual uint32_t GetCount() const = 0;

        static std::shared_ptr<IndexBuffer> Create(const uint32_t* indices, uint32_t count);
    };
}
