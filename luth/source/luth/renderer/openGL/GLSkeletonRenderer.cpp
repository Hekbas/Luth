#include "luthpch.h"
#include "luth/renderer/OpenGL/GLSkeletonRenderer.h"
#include "luth/renderer/SkinnedModel.h"

#include <glad/glad.h>

namespace Luth
{
    GLSkeletonRenderer::GLSkeletonRenderer() {
        // Create shader
        const char* vertexShaderSrc = R"(
            #version 460 core
            layout (location = 0) in vec3 aPosition;
            
            layout(std140, binding = 0) uniform TransformUBO {
                mat4 view;
                mat4 projection;
                mat4 model;
            };
            
            void main() {
                gl_Position = projection * view * vec4(aPosition, 1.0);
            }
        )";

        const char* fragmentShaderSrc = R"(
            #version 460 core
            out vec4 FragColor;
            
            uniform vec3 uColor;
            
            void main() {
                FragColor = vec4(uColor, 1.0);
            }
        )";

        m_Shader = std::make_shared<GLShader>(vertexShaderSrc, fragmentShaderSrc);

        // Generate buffers
        glGenVertexArrays(1, &m_VAO);
        glGenBuffers(1, &m_VBO);

        // Set up vertex attributes
        glBindVertexArray(m_VAO);
        glBindBuffer(GL_ARRAY_BUFFER, m_VBO);

        glEnableVertexAttribArray(0);
        glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, sizeof(glm::vec3), (void*)0);

        glBindVertexArray(0);
    }

    GLSkeletonRenderer::~GLSkeletonRenderer() {
        glDeleteVertexArrays(1, &m_VAO);
        glDeleteBuffers(1, &m_VBO);
    }

    void GLSkeletonRenderer::Update(const SkinnedModel& model) {
        const auto& hierarchy = model.GetBoneHierarchy();
        const auto& boneInfo = model.GetBoneTransforms();

        m_Positions.clear();

        // Collect bone positions
        for (const auto& node : hierarchy) {
            if (node.BoneIndex != -1) {
                const auto& transform = boneInfo[node.BoneIndex].FinalTransform;
                m_Positions.push_back(glm::vec3(transform[3]));
            }
        }

        // Create lines between bones and parents
        std::vector<glm::vec3> lines;
        for (const auto& node : hierarchy) {
            if (node.BoneIndex != -1 && node.ParentIndex != -1) {
                const auto& parentNode = hierarchy[node.ParentIndex];
                if (parentNode.BoneIndex != -1) {
                    // Add line from parent to this bone
                    lines.push_back(m_Positions[parentNode.BoneIndex]);
                    lines.push_back(m_Positions[node.BoneIndex]);
                }
            }
        }

        m_VertexCount = static_cast<uint32_t>(lines.size());

        // Update GPU buffer
        glBindVertexArray(m_VAO);
        glBindBuffer(GL_ARRAY_BUFFER, m_VBO);
        glBufferData(GL_ARRAY_BUFFER,
            lines.size() * sizeof(glm::vec3),
            lines.data(),
            GL_DYNAMIC_DRAW);
        glBindVertexArray(0);
    }

    void GLSkeletonRenderer::Draw() {
        if (m_VertexCount == 0) return;

        m_Shader->Bind();
        m_Shader->SetVec3("uColor", glm::vec3(1.0f, 0.0f, 0.0f)); // Red color

        glBindVertexArray(m_VAO);
        glDrawArrays(GL_LINES, 0, m_VertexCount);
        glBindVertexArray(0);

        m_Shader->Unbind();
    }
}