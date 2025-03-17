#pragma once

#include <Luth.h>

#include <imgui.h>

// TEST
#include <luth/resources/ShaderLibrary.h>
#include <luth/resources/ResourceManager.h>

#include <luth/renderer/Renderer.h>
#include <luth/renderer/Buffer.h>
#include <luth/renderer/Shader.h>
#include <luth/renderer/openGL/GLRendererAPI.h>
#include <luth/renderer/openGL/GLBuffer.h>
#include <memory>

// TEST OPENGL
#include <GLFW/glfw3.h>
#include <glm/glm.hpp>

namespace Luth
{
    struct GLVertex {
        glm::vec2 pos;
        glm::vec3 color;
        glm::vec2 texCoord;
    };

    class OpenGLApp : public App
    {
    public:
        OpenGLApp(int argc, char** argv);
        ~OpenGLApp() override = default;

    protected:
        void OnInit() override;
        void OnUpdate(f32 dt) override;
        void OnUIRender() override;
        void OnShutdown() override;

    private:
        void InitScreenQuad();
        void InitUniformBuffer();
        void UpdateUniforms(const Mat4& model, const Mat4& view, const Mat4& proj);
        void LoadShader();

        GLuint quadVAO, quadVBO;
        GLuint m_UniformBuffer;
        std::shared_ptr<Luth::Shader> shader;
        fs::path shaderPath;
    };
}
