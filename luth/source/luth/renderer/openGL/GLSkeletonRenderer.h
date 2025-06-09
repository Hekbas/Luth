#pragma once

#include "luth/renderer/SkeletonRenderer.h"
#include "luth/renderer/openGL/GLShader.h"

#include <vector>
#include <glm/glm.hpp>

namespace Luth
{
    class GLSkeletonRenderer final : public SkeletonRenderer
    {
    public:
        GLSkeletonRenderer();
        ~GLSkeletonRenderer() override;

        void Update(const SkinnedModel& model) override;
        void Draw() override;

    private:
        GLuint m_VAO = 0;
        GLuint m_VBO = 0;
        uint32_t m_VertexCount = 0;
        std::shared_ptr<GLShader> m_Shader;
        std::vector<glm::vec3> m_Positions;
    };
}
