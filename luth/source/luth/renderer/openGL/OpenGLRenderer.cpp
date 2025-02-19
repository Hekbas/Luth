#include "luthpch.h"
#include "luth/renderer/OpenGL/OpenGLRenderer.h"
#include "luth/core/Log.h"

#include <glad/glad.h>

#define LH_GL_CHECK_ERROR() CheckError(__FILE__, __LINE__)

namespace Luth
{
    void OpenGLRenderer::Init()
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

        // TODO: Look into fmt GLubyte*
        LH_CORE_INFO("OpenGL Renderer initialized");
        LH_CORE_INFO(" - Vendor: {:p}", fmt::ptr(glGetString(GL_VENDOR)));
        LH_CORE_INFO(" - Renderer: {:p}", fmt::ptr(glGetString(GL_RENDERER)));
        LH_CORE_INFO(" - Version: {:p}", fmt::ptr(glGetString(GL_VERSION)));
    }

    void OpenGLRenderer::Shutdown()
    {
        // Cleanup OpenGL-specific resources
    }

    void OpenGLRenderer::SetClearColor(const glm::vec4& color)
    {
        m_ClearColor = color;
        glClearColor(color.r, color.g, color.b, color.a);
        LH_GL_CHECK_ERROR();
    }

    void OpenGLRenderer::Clear()
    {
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
        LH_GL_CHECK_ERROR();
    }

    void OpenGLRenderer::SetViewport(u32 x, u32 y, u32 width, u32 height)
    {
        glViewport(x, y, width, height);
        LH_GL_CHECK_ERROR();
    }

    void OpenGLRenderer::EnableDepthTest(bool enable)
    {
        m_DepthTestEnabled = enable;
        enable ? glEnable(GL_DEPTH_TEST) : glDisable(GL_DEPTH_TEST);
        LH_GL_CHECK_ERROR();
    }

    void OpenGLRenderer::EnableBlending(bool enable)
    {
        m_BlendingEnabled = enable;
        enable ? glEnable(GL_BLEND) : glDisable(GL_BLEND);
        LH_GL_CHECK_ERROR();
    }

    void OpenGLRenderer::SetBlendFunction(u32 srcFactor, u32 dstFactor)
    {
        glBlendFunc(srcFactor, dstFactor);
        LH_GL_CHECK_ERROR();
    }

    void OpenGLRenderer::DrawIndexed(u32 count)
    {
        glDrawElements(GL_TRIANGLES, count, GL_UNSIGNED_INT, nullptr);
        LH_GL_CHECK_ERROR();
    }

    void OpenGLRenderer::CheckError(const char* file, int line)
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
