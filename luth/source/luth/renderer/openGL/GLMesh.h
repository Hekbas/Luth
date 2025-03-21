#pragma once

#include "luth/core/Log.h"
#include "luth/renderer/Mesh.h"
#include "luth/renderer/openGL/GLBuffer.h"
#include "luth/renderer/openGL/GLTexture.h"

#include <glad/glad.h>
#include <vector>
#include <memory>

namespace Luth
{
    class GLMesh : public Mesh
    {
    public:
        GLMesh(const std::shared_ptr<GLVertexBuffer>& vertexBuffer,
            const std::shared_ptr<GLIndexBuffer>& indexBuffer,
            const std::vector<std::shared_ptr<Texture>>* textures);

        void Bind() const override;
        void Draw() const override;

    private:
        void CreateVAO();
        static GLenum ShaderDataTypeToGLType(ShaderDataType type);

        GLuint m_VAO;
        std::shared_ptr<GLVertexBuffer> m_VertexBuffer;
        std::shared_ptr<GLIndexBuffer> m_IndexBuffer;
        std::vector<std::shared_ptr<Texture>> m_Textures;
    };
}
