#include "Luthpch.h"
#include "luth/renderer/pipeline/passes/GeometryPass.h"
#include "luth/renderer/pipeline/RenderUtils.h"

namespace Luth
{
	void GeometryPass::Init(u32 w, u32 h)
	{
		m_GeoShader = ShaderLibrary::Get("LuthDeferredGeo");
		m_GeoFBO = Framebuffer::Create({
			.Width = w, .Height = h,
			.ColorAttachments = {
				{ .InternalFormat = GL_RGB16F,	.Name = "Position" },
				{ .InternalFormat = GL_RGB16F,	.Name = "Normal"   },
				{ .InternalFormat = GL_RGBA16F, .Name = "Albedo"   },
				{ .InternalFormat = GL_RGB16F,	.Name = "MRAO"	   },
				{ .InternalFormat = GL_RGBA16F, .Name = "ET"       },
				{ .InternalFormat = GL_RGB16F,	.Name = "Bone"     }
			},
			.DepthStencilAttachment = {{ .InternalFormat = GL_DEPTH24_STENCIL8 }}
		});

		// Prevent light from leaking
		Renderer::SetClearColor(Vec4(0));
	}

	void GeometryPass::Resize(u32 w, u32 h) {
		m_GeoFBO->Resize(w, h);
	}

	void GeometryPass::Execute(const RenderContext& ctx)
	{
		m_GeoFBO->Bind();
		Renderer::Clear(BufferBit::Color | BufferBit::Depth);
		Renderer::EnableBlending(false);

		m_GeoShader->Bind();
		for (auto& cmd : ctx.opaque) {
			m_GeoShader->SetBool("u_IsSkinned", cmd.meshRend->isSkinned);
			RenderUtils::DrawCommand(cmd, *m_GeoShader);
		}
		m_GeoFBO->Unbind();
	}
}
