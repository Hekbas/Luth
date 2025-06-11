#include "Luthpch.h"
#include "luth/renderer/pipeline/RenderPipeline.h"

namespace Luth
{
    void RenderPipeline::InitAll(u32 w, u32 h) {
        m_Width = w; m_Height = h;
        for (auto& p : m_Passes) p->Init(w, h);
    }

    void RenderPipeline::ResizeAll(u32 w, u32 h) {
        m_Width = w; m_Height = h;
        for (auto& p : m_Passes) p->Resize(w, h);
    }

    void RenderPipeline::RenderAll(const RenderContext& ctx) {
        for (auto& p : m_Passes)
            p->Execute(ctx);
    }
}
