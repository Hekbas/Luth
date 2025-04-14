#pragma once

#include "luth/renderer/Framebuffer.h"

#include <vector>
#include <functional>

namespace Luth
{
    class RenderPipeline
    {
    public:
        struct RenderPass {
            std::string Name;
            std::shared_ptr<Framebuffer> TargetFBO;
            std::vector<std::string> InputAttachments;
            std::function<void()> Execute;
        };

        static std::shared_ptr<RenderPipeline> Create();

        void AddPass(const RenderPass& pass);
        void Execute();
        void Resize(uint32_t width, uint32_t height);

        std::shared_ptr<Framebuffer> GetFramebuffer(const std::string& name) const;

    private:
        std::vector<RenderPass> m_Passes;
        std::unordered_map<std::string, std::shared_ptr<Framebuffer>> m_NamedFramebuffers;
    };
}
