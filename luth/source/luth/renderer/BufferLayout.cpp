#include "luthpch.h"
#include "BufferLayout.h"
#include "Renderer.h"

#include <vulkan/vulkan.h>

namespace Luth
{
    static uint32_t ShaderDataTypeSize(ShaderDataType type)
    {
        switch (type)
        {
            case ShaderDataType::Bool:    return 1;
            case ShaderDataType::Int:     return 4;
            case ShaderDataType::Int2:    return 4 * 2;
            case ShaderDataType::Int3:    return 4 * 3;
            case ShaderDataType::Int4:    return 4 * 4;
            case ShaderDataType::Float:   return 4;
            case ShaderDataType::Float2:  return 4 * 2;
            case ShaderDataType::Float3:  return 4 * 3;
            case ShaderDataType::Float4:  return 4 * 4;
            case ShaderDataType::Mat3:    return 4 * 3 * 3;
            case ShaderDataType::Mat4:    return 4 * 4 * 4;
            default:
                LH_CORE_ASSERT(false, "Unknown ShaderDataType!");
                return 0;
        }
    }

    BufferElement::BufferElement(ShaderDataType type, const std::string& name, bool normalized)
        : Name(name), Type(type), Size(ShaderDataTypeSize(type)),
        Offset(0), Normalized(normalized) {}

    uint32_t BufferElement::GetComponentCount() const
    {
        switch (Type)
        {
            case ShaderDataType::Bool:    return 1;
            case ShaderDataType::Int:     return 1;
            case ShaderDataType::Int2:    return 2;
            case ShaderDataType::Int3:    return 3;
            case ShaderDataType::Int4:    return 4;
            case ShaderDataType::Float:   return 1;
            case ShaderDataType::Float2:  return 2;
            case ShaderDataType::Float3:  return 3;
            case ShaderDataType::Float4:  return 4;
            case ShaderDataType::Mat3:    return 3 * 3;
            case ShaderDataType::Mat4:    return 4 * 4;
            default:
                LH_CORE_ASSERT(false, "Unknown ShaderDataType!");
                return 0;
        }
    }

    BufferLayout::BufferLayout(std::initializer_list<BufferElement> elements)
        : m_Elements(elements) {
        CalculateOffsetsAndStride();
    }

    void BufferLayout::CalculateOffsetsAndStride()
    {
        uint32_t offset = 0;
        m_Stride = 0;
        for (auto& element : m_Elements) {
            element.Offset = offset;
            offset += element.Size;
            m_Stride += element.Size;
        }
    }

    // Vulkan-specific implementations
    std::vector<VkVertexInputBindingDescription> BufferLayout::GetBindingDescriptions() const
    {
        std::vector<VkVertexInputBindingDescription> descriptions;

        VkVertexInputBindingDescription bindingDescription{};
        bindingDescription.binding = 0;
        bindingDescription.stride = m_Stride;
        bindingDescription.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;

        descriptions.push_back(bindingDescription);
        return descriptions;
    }

    std::vector<VkVertexInputAttributeDescription> BufferLayout::GetAttributeDescriptions() const
    {
        std::vector<VkVertexInputAttributeDescription> descriptions;
        uint32_t location = 0;

        for (const auto& element : m_Elements) {
            VkVertexInputAttributeDescription attributeDescription{};
            attributeDescription.binding = 0;
            attributeDescription.location = location++;
            attributeDescription.format = [&]() {
                switch (element.Type) {
                    case ShaderDataType::Bool:   return VK_FORMAT_R8_UINT;
                    case ShaderDataType::Int:    return VK_FORMAT_R32_SINT;
                    case ShaderDataType::Int2:   return VK_FORMAT_R32G32_SINT;
                    case ShaderDataType::Int3:   return VK_FORMAT_R32G32B32_SINT;
                    case ShaderDataType::Int4:   return VK_FORMAT_R32G32B32A32_SINT;
                    case ShaderDataType::Float:  return VK_FORMAT_R32_SFLOAT;
                    case ShaderDataType::Float2: return VK_FORMAT_R32G32_SFLOAT;
                    case ShaderDataType::Float3: return VK_FORMAT_R32G32B32_SFLOAT;
                    case ShaderDataType::Float4: return VK_FORMAT_R32G32B32A32_SFLOAT;
                    case ShaderDataType::Mat3:   return VK_FORMAT_R32G32B32_SFLOAT;
                    case ShaderDataType::Mat4:   return VK_FORMAT_R32G32B32A32_SFLOAT;
                    default:
                        LH_CORE_ASSERT(false, "Unknown ShaderDataType!");
                        return VK_FORMAT_UNDEFINED;
                }
            }();
            attributeDescription.offset = element.Offset;

            descriptions.push_back(attributeDescription);
        }
        return descriptions;
    }
}
