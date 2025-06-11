#pragma once

#include "luth/core/LuthTypes.h"
#include "luth/renderer/pipeline/RenderPass.h"
#include "luth/renderer/pipeline/passes/GeometryPass.h"
#include "luth/renderer/pipeline/passes/LightingPass.h"
#include "luth/renderer/pipeline/passes/TransparentPass.h"
#include "luth/renderer/pipeline/passes/SSAOPass.h"
#include "luth/renderer/pipeline/passes/PostProcessPass.h"

#include <memory>
#include <vector>

namespace Luth
{
    class RenderPipeline
    {
    public:
        template<typename T, typename... Args>
        void AddPass(Args&&... args) {
            m_Passes.emplace_back(std::make_unique<T>(std::forward<Args>(args)...));
        }

        void InitAll(u32 width, u32 height);
        void ResizeAll(u32 width, u32 height);
        void RenderAll(const RenderContext& ctx);

        // Find the first pass of type T
        template<typename T>
        T* GetPass() const {
            for (auto& p : m_Passes) {
                if (auto casted = dynamic_cast<T*>(p.get()))
                    return casted;
            }
            return nullptr;
        }

        // Query final color attachment from PostProcessPass
        u32 GetFinalColorAttachment() const {
            if (auto pp = GetPass<PostProcessPass>())
				return pp->GetFinalColorAttachment();
            return 0;
        }

        // Collect attachments from all passes
        std::vector<std::pair<std::string, u32>> GetAllAttachments() const {
            std::vector<std::pair<std::string, u32>> allAttachments;

            for (auto& pass : m_Passes) {
                auto attachments = pass->GetAllAttachments();
                allAttachments.insert(allAttachments.end(),
                    attachments.begin(), attachments.end());
            }
            return allAttachments;
        }

        // Lookup an attachment by its name
        u32 GetAttachmentByName(const std::string& name) const {
            if (name == "Final") return GetFinalColorAttachment();
            for (auto& [label, id] : GetAllAttachments())
                if (label == name) return id;
            return 0;
        }

    private:
        std::vector<std::unique_ptr<RenderPass>> m_Passes;
        u32 m_Width = 0, m_Height = 0;
    };
}
