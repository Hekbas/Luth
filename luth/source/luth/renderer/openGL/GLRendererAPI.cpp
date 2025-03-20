#include "luthpch.h"
#include "luth/renderer/OpenGL/GLRendererAPI.h"
#include "luth/core/Log.h"

#include <glad/glad.h>

#define LH_GL_CHECK_ERROR() CheckError(__FILE__, __LINE__)

namespace Luth
{
    void GLRendererAPI::Init()
    {
        // Glad should already be initialized by Window class
        // Verify GLAD loaded properly
        if (!gladLoadGL()) {
            LH_CORE_CRITICAL("Failed to initialize Glad!");
            return;
        }

        EnableDepthTest(true);
        EnableBlending(true);
        SetBlendFunction(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

        LH_CORE_INFO("OpenGL Renderer initialized");
        LH_CORE_INFO(" - Vendor: {0}", reinterpret_cast<const char*>(glGetString(GL_VENDOR)));
        LH_CORE_INFO(" - Renderer: {0}", reinterpret_cast<const char*>(glGetString(GL_RENDERER)));
        LH_CORE_INFO(" - Version: {0}", reinterpret_cast<const char*>(glGetString(GL_VERSION)));
    }

    void GLRendererAPI::Shutdown()
    {
        glBindVertexArray(0);
        glBindBuffer(GL_ARRAY_BUFFER, 0);
        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);

        m_Meshes.clear();
    }

    void GLRendererAPI::SetClearColor(const glm::vec4& color)
    {
        m_ClearColor = color;
        glClearColor(color.r, color.g, color.b, color.a);
        LH_GL_CHECK_ERROR();
    }

    void GLRendererAPI::Clear()
    {
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
        LH_GL_CHECK_ERROR();
    }

    void GLRendererAPI::SetViewport(u32 x, u32 y, u32 width, u32 height)
    {
        glViewport(x, y, width, height);
        LH_GL_CHECK_ERROR();
    }

    void GLRendererAPI::EnableDepthTest(bool enable)
    {
        m_DepthTestEnabled = enable;
        enable ? glEnable(GL_DEPTH_TEST) : glDisable(GL_DEPTH_TEST);
        LH_GL_CHECK_ERROR();
    }

    void GLRendererAPI::EnableBlending(bool enable)
    {
        m_BlendingEnabled = enable;
        enable ? glEnable(GL_BLEND) : glDisable(GL_BLEND);
        LH_GL_CHECK_ERROR();
    }

    void GLRendererAPI::SetBlendFunction(u32 srcFactor, u32 dstFactor)
    {
        glBlendFunc(srcFactor, dstFactor);
        LH_GL_CHECK_ERROR();
    }

    void GLRendererAPI::SubmitMesh(const std::shared_ptr<Mesh>& mesh)
    {
        auto glMesh = std::dynamic_pointer_cast<GLMesh>(mesh);
        if (!glMesh) {
            LH_CORE_WARN("GLRendererAPI::SubmitMesh - Invalid mesh type submitted!");
            return;
        }

        m_Meshes.push_back(glMesh);
    }

    void GLRendererAPI::DrawIndexed(u32 count)
    {
        glDrawElements(GL_TRIANGLES, count, GL_UNSIGNED_INT, nullptr);
        LH_GL_CHECK_ERROR();
    }

    void GLRendererAPI::DrawFrame()
    {
        for (auto mesh : m_Meshes)
        {
            mesh->Bind();
            mesh->Draw();
        }
    }

    void GLRendererAPI::CheckError(const char* file, int line)
    {
        while (GLenum error = glGetError())
        {
            std::string errorStr;
            switch (error)
            {
                case GL_INVALID_ENUM:       errorStr = "INVALID_ENUM";  break;
                case GL_INVALID_VALUE:      errorStr = "INVALID_VALUE"; break;
                case GL_INVALID_OPERATION:  errorStr = "INVALID_OPERATION"; break;
                case GL_OUT_OF_MEMORY:      errorStr = "OUT_OF_MEMORY"; break;
                default:                    errorStr = "UNKNOWN_ERROR"; break;
            }
            LH_CORE_ERROR("OpenGL Error ({0}) at {1}:{2}", errorStr, file, line);
        }
    }
}
