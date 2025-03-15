#pragma once

#include <vulkan/vulkan.h>
#include <vector>

namespace Luth
{
    class VKSync
    {
    public:
        struct FrameData
        {
            VkSemaphore imageAvailable;
            VkSemaphore renderFinished;
            VkFence inFlightFence;
        };

        VKSync(VkDevice device, size_t maxFramesInFlight);
        ~VKSync();

        FrameData& GetCurrentFrame() { return m_Frames[m_CurrentFrame]; }
        void AdvanceFrame() { m_CurrentFrame = (m_CurrentFrame + 1) % m_MaxFrames; }

        uint32_t GetCurrentFrameIndex() const { return m_CurrentFrame; }

    private:
        VkDevice m_Device;
        std::vector<FrameData> m_Frames;
        size_t m_CurrentFrame = 0;
        size_t m_MaxFrames;
    };
}
