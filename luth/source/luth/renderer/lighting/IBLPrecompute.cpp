#include "luthpch.h"
#include "luth/renderer/lighting/IBLPrecompute.h"
#include "luth/renderer/shader/ShaderLibrary.h"
#include "luth/renderer/backend/vulkan/VulkanTexture.h"
#include "luth/renderer/backend/vulkan/VulkanContext.h"
#include "luth/renderer/backend/vulkan/VulkanAllocator.h"
#include "luth/renderer/backend/vulkan/VulkanComputePipeline.h"
#include "luth/resources/FileSystem.h"

#include <stb/stb_image.h>
#include <vma/vk_mem_alloc.h>

namespace Luth
{
    // =========================================================================
    // Internal helpers
    // =========================================================================

    static void TransitionImage(VkCommandBuffer cmd, VkImage image, VkImageLayout oldLayout, VkImageLayout newLayout,
        VkAccessFlags srcAccess, VkAccessFlags dstAccess,
        VkPipelineStageFlags srcStage, VkPipelineStageFlags dstStage,
        u32 mipLevels, u32 layerCount, VkImageAspectFlags aspect = VK_IMAGE_ASPECT_COLOR_BIT)
    {
        VkImageMemoryBarrier barrier{ VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
        barrier.oldLayout = oldLayout;
        barrier.newLayout = newLayout;
        barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.image = image;
        barrier.subresourceRange.aspectMask = aspect;
        barrier.subresourceRange.baseMipLevel = 0;
        barrier.subresourceRange.levelCount = mipLevels;
        barrier.subresourceRange.baseArrayLayer = 0;
        barrier.subresourceRange.layerCount = layerCount;
        barrier.srcAccessMask = srcAccess;
        barrier.dstAccessMask = dstAccess;
        vkCmdPipelineBarrier(cmd, srcStage, dstStage, 0, 0, nullptr, 0, nullptr, 1, &barrier);
    }

    // Local descriptor pool for the precompute compute passes (equirect→cubemap, irradiance,
    // 5 prefilter mips, BRDF LUT). One-shot pool destroyed at the end of Precompute — replaces
    // the global DescriptorAllocator for which IBL was the only consumer.
    static VkDescriptorPool CreateIBLDescriptorPool(VkDevice device, u32 maxSets)
    {
        VkDescriptorPoolSize sizes[] = {
            { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, maxSets * 2 },
            { VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,          maxSets * 2 },
        };
        VkDescriptorPoolCreateInfo poolInfo{ VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO };
        poolInfo.maxSets = maxSets;
        poolInfo.poolSizeCount = (u32)std::size(sizes);
        poolInfo.pPoolSizes = sizes;
        VkDescriptorPool pool = VK_NULL_HANDLE;
        vkCreateDescriptorPool(device, &poolInfo, nullptr, &pool);
        return pool;
    }

    static VkDescriptorSet AllocateIBLSet(VkDevice device, VkDescriptorPool pool, VkDescriptorSetLayout layout)
    {
        VkDescriptorSetAllocateInfo allocInfo{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
        allocInfo.descriptorPool = pool;
        allocInfo.descriptorSetCount = 1;
        allocInfo.pSetLayouts = &layout;
        VkDescriptorSet set = VK_NULL_HANDLE;
        vkAllocateDescriptorSets(device, &allocInfo, &set);
        return set;
    }


    // =========================================================================
    // IBL::Precompute
    // =========================================================================

    namespace IBL
    {
        IBLResult Precompute(const std::filesystem::path& hdrPath)
        {
            IBLResult result;
            VkDevice device = VulkanContext::Get().GetDevice();

            // ---- 1. Load HDR environment map ----
            int hdrW, hdrH, hdrChannels;
            stbi_set_flip_vertically_on_load(1);
            float* hdrData = stbi_loadf(hdrPath.string().c_str(), &hdrW, &hdrH, &hdrChannels, 4);
            if (!hdrData)
            {
                LH_CORE_WARN("IBL: No HDR environment found at '{}'. IBL disabled.", hdrPath.string());
                result.irradianceMap  = std::make_shared<VKTexture>(1, 1, TextureFormat::RGBA16F, 6,
                    VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT, 1);
                result.prefilteredMap = std::make_shared<VKTexture>(1, 1, TextureFormat::RGBA16F, 6,
                    VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT, 1);
                result.brdfLut = std::make_shared<VKTexture>(1, 1, TextureFormat::RG16F, 1, 0, 1);
            }
            else
            {
                LH_CORE_INFO("IBL: Loaded HDR environment {}x{} from '{}'", hdrW, hdrH, hdrPath.string());

                // Local pool sized for the 8 sets used below: equirect (1), irradiance (1),
                // prefilter mips (5), BRDF LUT (1). Destroyed at the end of this branch — all
                // consuming dispatches run synchronously via ImmediateSubmit, so GPU work is
                // complete by then.
                VkDescriptorPool iblPool = CreateIBLDescriptorPool(device, 8);

                // ---- 2. Upload HDR as 2D staging texture ----
                auto hdrStaging = std::make_shared<VKTexture>((u32)hdrW, (u32)hdrH, TextureFormat::RGBA32F,
                    1, 0, 1, VkImageUsageFlags(0));
                {
                    VkDeviceSize imageSize = (VkDeviceSize)hdrW * hdrH * 4 * sizeof(float);
                    VkBuffer stagingBuffer;
                    VkBufferCreateInfo bufferInfo = { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
                    bufferInfo.size = imageSize;
                    bufferInfo.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
                    VmaAllocation stagingAlloc = VulkanAllocator::AllocateBuffer(bufferInfo, VMA_MEMORY_USAGE_CPU_ONLY, stagingBuffer);
                    void* mapped = VulkanAllocator::Map(stagingAlloc);
                    memcpy(mapped, hdrData, (size_t)imageSize);
                    VulkanAllocator::Unmap(stagingAlloc);

                    VulkanContext::Get().ImmediateSubmit([&](VkCommandBuffer cmd) {
                        TransitionImage(cmd, hdrStaging->GetImage(),
                            VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                            0, VK_ACCESS_TRANSFER_WRITE_BIT,
                            VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 1, 1);

                        VkBufferImageCopy region{};
                        region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
                        region.imageSubresource.layerCount = 1;
                        region.imageExtent = { (u32)hdrW, (u32)hdrH, 1 };
                        vkCmdCopyBufferToImage(cmd, stagingBuffer, hdrStaging->GetImage(),
                            VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);

                        TransitionImage(cmd, hdrStaging->GetImage(),
                            VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                            VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT,
                            VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 1, 1);
                    });
                    VulkanAllocator::FreeBuffer(stagingBuffer, stagingAlloc);
                }
                stbi_image_free(hdrData);

                // ---- 3. Create environment cubemap (1024x1024) ----
                const u32 envSize = 1024;
                const u32 envMips = static_cast<u32>(std::floor(std::log2(envSize))) + 1;
                auto envCubemap = std::make_shared<VKTexture>(envSize, envSize, TextureFormat::RGBA16F, 6,
                    VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT, envMips, VK_IMAGE_USAGE_STORAGE_BIT);

                // ---- 4. Equirect → Cubemap conversion ----
                {
                    auto sh = ShaderLibrary::LoadEngine("shaders/equirect_to_cubemap.comp");
                    auto spv = sh ? sh->GetSpirV() : std::vector<u32>{};

                    VkDescriptorSetLayoutBinding layoutBindings[2] = {};
                    layoutBindings[0] = { 0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr };
                    layoutBindings[1] = { 1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr };

                    VkDescriptorSetLayoutCreateInfo layoutCI{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
                    layoutCI.bindingCount = 2;
                    layoutCI.pBindings = layoutBindings;
                    VkDescriptorSetLayout descLayout;
                    vkCreateDescriptorSetLayout(device, &layoutCI, nullptr, &descLayout);

                    VkDescriptorSet descSet = AllocateIBLSet(device, iblPool, descLayout);

                    VkSamplerCreateInfo sampCI{ VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO };
                    sampCI.magFilter = VK_FILTER_LINEAR;
                    sampCI.minFilter = VK_FILTER_LINEAR;
                    sampCI.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
                    sampCI.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
                    sampCI.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
                    VkSampler hdrSampler;
                    vkCreateSampler(device, &sampCI, nullptr, &hdrSampler);

                    VulkanContext::Get().ImmediateSubmit([&](VkCommandBuffer cmd) {
                        TransitionImage(cmd, envCubemap->GetImage(),
                            VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL,
                            0, VK_ACCESS_SHADER_WRITE_BIT,
                            VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                            envMips, 6);
                    });

                    VkImageView envMip0View = envCubemap->CreateMipView(0, true);

                    VkDescriptorImageInfo hdrInfo{};
                    hdrInfo.sampler = hdrSampler;
                    hdrInfo.imageView = hdrStaging->GetImageView();
                    hdrInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

                    VkDescriptorImageInfo cubemapInfo{};
                    cubemapInfo.imageView = envMip0View;
                    cubemapInfo.imageLayout = VK_IMAGE_LAYOUT_GENERAL;

                    VkWriteDescriptorSet writes[2] = {};
                    writes[0] = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
                    writes[0].dstSet = descSet;
                    writes[0].dstBinding = 0;
                    writes[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
                    writes[0].descriptorCount = 1;
                    writes[0].pImageInfo = &hdrInfo;

                    writes[1] = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
                    writes[1].dstSet = descSet;
                    writes[1].dstBinding = 1;
                    writes[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
                    writes[1].descriptorCount = 1;
                    writes[1].pImageInfo = &cubemapInfo;

                    vkUpdateDescriptorSets(device, 2, writes, 0, nullptr);

                    {
                        VKComputePipeline equirectPipeline(spv, { descLayout });
                        VulkanContext::Get().ImmediateSubmit([&](VkCommandBuffer cmd) {
                            equirectPipeline.Bind(cmd);
                            vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE,
                                equirectPipeline.GetLayout(), 0, 1, &descSet, 0, nullptr);
                            vkCmdDispatch(cmd, (envSize + 15) / 16, (envSize + 15) / 16, 6);
                        });
                    }

                    VulkanContext::Get().ImmediateSubmit([&](VkCommandBuffer cmd) {
                        TransitionImage(cmd, envCubemap->GetImage(),
                            VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                            VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT,
                            VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                            envMips, 6);
                    });

                    // Generate mipmaps for environment cubemap
                    VulkanContext::Get().ImmediateSubmit([&](VkCommandBuffer cmd) {
                        i32 mipW = envSize, mipH = envSize;
                        for (u32 i = 1; i < envMips; i++) {
                            VkImageMemoryBarrier barrier{ VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
                            barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
                            barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
                            barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
                            barrier.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
                            barrier.image = envCubemap->GetImage();
                            barrier.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, i - 1, 1, 0, 6 };
                            barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                            barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                            vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                                0, 0, nullptr, 0, nullptr, 1, &barrier);

                            i32 nextW = mipW > 1 ? mipW / 2 : 1;
                            i32 nextH = mipH > 1 ? mipH / 2 : 1;

                            VkImageBlit blit{};
                            blit.srcOffsets[0] = { 0, 0, 0 };
                            blit.srcOffsets[1] = { mipW, mipH, 1 };
                            blit.srcSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, i - 1, 0, 6 };
                            blit.dstOffsets[0] = { 0, 0, 0 };
                            blit.dstOffsets[1] = { nextW, nextH, 1 };
                            blit.dstSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, i, 0, 6 };
                            vkCmdBlitImage(cmd, envCubemap->GetImage(), VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                                envCubemap->GetImage(), VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                1, &blit, VK_FILTER_LINEAR);

                            barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
                            barrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
                            barrier.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
                            barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
                            vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                                0, 0, nullptr, 0, nullptr, 1, &barrier);

                            mipW = nextW;
                            mipH = nextH;
                        }
                        // Last mip: DST → SHADER_READ_ONLY
                        VkImageMemoryBarrier barrier{ VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
                        barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
                        barrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
                        barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
                        barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
                        barrier.image = envCubemap->GetImage();
                        barrier.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, envMips - 1, 1, 0, 6 };
                        barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                        barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                            0, 0, nullptr, 0, nullptr, 1, &barrier);
                    });

                    vkDestroyImageView(device, envMip0View, nullptr);
                    vkDestroySampler(device, hdrSampler, nullptr);
                    vkDestroyDescriptorSetLayout(device, descLayout, nullptr);
                }

                // ---- 5. Irradiance convolution (32x32 cubemap) ----
                {
                    const u32 irrSize = 32;
                    result.irradianceMap = std::make_shared<VKTexture>(irrSize, irrSize, TextureFormat::RGBA16F, 6,
                        VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT, 1, VK_IMAGE_USAGE_STORAGE_BIT);

                    auto sh = ShaderLibrary::LoadEngine("shaders/irradiance_convolve.comp");
                    auto spv = sh ? sh->GetSpirV() : std::vector<u32>{};

                    VkDescriptorSetLayoutBinding layoutBindings[2] = {};
                    layoutBindings[0] = { 0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr };
                    layoutBindings[1] = { 1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr };

                    VkDescriptorSetLayoutCreateInfo layoutCI{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
                    layoutCI.bindingCount = 2;
                    layoutCI.pBindings = layoutBindings;
                    VkDescriptorSetLayout descLayout;
                    vkCreateDescriptorSetLayout(device, &layoutCI, nullptr, &descLayout);

                    VkDescriptorSet descSet = AllocateIBLSet(device, iblPool, descLayout);

                    VkSamplerCreateInfo sampCI{ VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO };
                    sampCI.magFilter = VK_FILTER_LINEAR;
                    sampCI.minFilter = VK_FILTER_LINEAR;
                    sampCI.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
                    sampCI.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
                    sampCI.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
                    sampCI.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
                    sampCI.maxLod = (float)envMips;
                    VkSampler envSampler;
                    vkCreateSampler(device, &sampCI, nullptr, &envSampler);

                    VulkanContext::Get().ImmediateSubmit([&](VkCommandBuffer cmd) {
                        TransitionImage(cmd, std::static_pointer_cast<VKTexture>(result.irradianceMap)->GetImage(),
                            VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL,
                            0, VK_ACCESS_SHADER_WRITE_BIT,
                            VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 1, 6);
                    });

                    VkDescriptorImageInfo envInfo{};
                    envInfo.sampler = envSampler;
                    envInfo.imageView = envCubemap->GetImageView();
                    envInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

                    auto vkIrr = std::static_pointer_cast<VKTexture>(result.irradianceMap);
                    VkImageView irrStorageView = vkIrr->CreateMipView(0, true);

                    VkDescriptorImageInfo irrInfo{};
                    irrInfo.imageView = irrStorageView;
                    irrInfo.imageLayout = VK_IMAGE_LAYOUT_GENERAL;

                    VkWriteDescriptorSet writes[2] = {};
                    writes[0] = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
                    writes[0].dstSet = descSet;
                    writes[0].dstBinding = 0;
                    writes[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
                    writes[0].descriptorCount = 1;
                    writes[0].pImageInfo = &envInfo;
                    writes[1] = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
                    writes[1].dstSet = descSet;
                    writes[1].dstBinding = 1;
                    writes[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
                    writes[1].descriptorCount = 1;
                    writes[1].pImageInfo = &irrInfo;
                    vkUpdateDescriptorSets(device, 2, writes, 0, nullptr);

                    {
                        VKComputePipeline irrPipeline(spv, { descLayout });
                        VulkanContext::Get().ImmediateSubmit([&](VkCommandBuffer cmd) {
                            irrPipeline.Bind(cmd);
                            vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE,
                                irrPipeline.GetLayout(), 0, 1, &descSet, 0, nullptr);
                            vkCmdDispatch(cmd, (irrSize + 7) / 8, (irrSize + 7) / 8, 6);
                        });
                    }

                    vkDestroyImageView(device, irrStorageView, nullptr);

                    VulkanContext::Get().ImmediateSubmit([&](VkCommandBuffer cmd) {
                        TransitionImage(cmd, std::static_pointer_cast<VKTexture>(result.irradianceMap)->GetImage(),
                            VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                            VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT,
                            VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 1, 6);
                    });

                    vkDestroySampler(device, envSampler, nullptr);
                    vkDestroyDescriptorSetLayout(device, descLayout, nullptr);
                }

                // ---- 6. Pre-filtered environment map (128x128, 5 mip levels) ----
                {
                    const u32 pfSize = 128;
                    const u32 pfMips = 5;
                    result.prefilteredMap = std::make_shared<VKTexture>(pfSize, pfSize, TextureFormat::RGBA16F, 6,
                        VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT, pfMips, VK_IMAGE_USAGE_STORAGE_BIT);

                    auto sh = ShaderLibrary::LoadEngine("shaders/prefilter_env.comp");
                    auto spv = sh ? sh->GetSpirV() : std::vector<u32>{};

                    VkDescriptorSetLayoutBinding layoutBindings[2] = {};
                    layoutBindings[0] = { 0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr };
                    layoutBindings[1] = { 1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr };

                    VkDescriptorSetLayoutCreateInfo layoutCI{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
                    layoutCI.bindingCount = 2;
                    layoutCI.pBindings = layoutBindings;
                    VkDescriptorSetLayout descLayout;
                    vkCreateDescriptorSetLayout(device, &layoutCI, nullptr, &descLayout);

                    VkPushConstantRange pcRange{};
                    pcRange.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
                    pcRange.offset = 0;
                    pcRange.size = sizeof(float);

                    VKComputePipeline pfPipeline(spv, { descLayout }, { pcRange });

                    VkSamplerCreateInfo sampCI{ VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO };
                    sampCI.magFilter = VK_FILTER_LINEAR;
                    sampCI.minFilter = VK_FILTER_LINEAR;
                    sampCI.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
                    sampCI.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
                    sampCI.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
                    sampCI.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
                    sampCI.maxLod = (float)envMips;
                    VkSampler envSampler;
                    vkCreateSampler(device, &sampCI, nullptr, &envSampler);

                    auto vkPf = std::static_pointer_cast<VKTexture>(result.prefilteredMap);

                    VulkanContext::Get().ImmediateSubmit([&](VkCommandBuffer cmd) {
                        TransitionImage(cmd, vkPf->GetImage(),
                            VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL,
                            0, VK_ACCESS_SHADER_WRITE_BIT,
                            VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                            pfMips, 6);
                    });

                    for (u32 mip = 0; mip < pfMips; mip++)
                    {
                        u32 mipSize = pfSize >> mip;
                        float roughness = (float)mip / (float)(pfMips - 1);

                        VkImageView mipView = vkPf->CreateMipView(mip, true);

                        VkDescriptorSet descSet = AllocateIBLSet(device, iblPool, descLayout);

                        VkDescriptorImageInfo envInfo{};
                        envInfo.sampler = envSampler;
                        envInfo.imageView = envCubemap->GetImageView();
                        envInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

                        VkDescriptorImageInfo pfInfo{};
                        pfInfo.imageView = mipView;
                        pfInfo.imageLayout = VK_IMAGE_LAYOUT_GENERAL;

                        VkWriteDescriptorSet writes[2] = {};
                        writes[0] = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
                        writes[0].dstSet = descSet;
                        writes[0].dstBinding = 0;
                        writes[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
                        writes[0].descriptorCount = 1;
                        writes[0].pImageInfo = &envInfo;
                        writes[1] = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
                        writes[1].dstSet = descSet;
                        writes[1].dstBinding = 1;
                        writes[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
                        writes[1].descriptorCount = 1;
                        writes[1].pImageInfo = &pfInfo;
                        vkUpdateDescriptorSets(device, 2, writes, 0, nullptr);

                        VulkanContext::Get().ImmediateSubmit([&](VkCommandBuffer cmd) {
                            pfPipeline.Bind(cmd);
                            vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE,
                                pfPipeline.GetLayout(), 0, 1, &descSet, 0, nullptr);
                            vkCmdPushConstants(cmd, pfPipeline.GetLayout(),
                                VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(float), &roughness);
                            vkCmdDispatch(cmd, (mipSize + 15) / 16, (mipSize + 15) / 16, 6);
                        });

                        vkDestroyImageView(device, mipView, nullptr);
                    }

                    VulkanContext::Get().ImmediateSubmit([&](VkCommandBuffer cmd) {
                        TransitionImage(cmd, vkPf->GetImage(),
                            VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                            VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT,
                            VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                            pfMips, 6);
                    });

                    vkDestroySampler(device, envSampler, nullptr);
                    vkDestroyDescriptorSetLayout(device, descLayout, nullptr);
                }

                // ---- 7. BRDF LUT (512x512, RG16F) ----
                {
                    const u32 lutSize = 512;
                    result.brdfLut = std::make_shared<VKTexture>(lutSize, lutSize, TextureFormat::RG16F, 1, 0, 1,
                        VK_IMAGE_USAGE_STORAGE_BIT);

                    auto sh = ShaderLibrary::LoadEngine("shaders/brdf_lut.comp");
                    auto spv = sh ? sh->GetSpirV() : std::vector<u32>{};

                    VkDescriptorSetLayoutBinding layoutBinding = { 0, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr };

                    VkDescriptorSetLayoutCreateInfo layoutCI{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
                    layoutCI.bindingCount = 1;
                    layoutCI.pBindings = &layoutBinding;
                    VkDescriptorSetLayout descLayout;
                    vkCreateDescriptorSetLayout(device, &layoutCI, nullptr, &descLayout);

                    VkDescriptorSet descSet = AllocateIBLSet(device, iblPool, descLayout);

                    auto vkLut = std::static_pointer_cast<VKTexture>(result.brdfLut);

                    VulkanContext::Get().ImmediateSubmit([&](VkCommandBuffer cmd) {
                        TransitionImage(cmd, vkLut->GetImage(),
                            VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL,
                            0, VK_ACCESS_SHADER_WRITE_BIT,
                            VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 1, 1);
                    });

                    VkDescriptorImageInfo lutInfo{};
                    lutInfo.imageView = vkLut->GetImageView();
                    lutInfo.imageLayout = VK_IMAGE_LAYOUT_GENERAL;

                    VkWriteDescriptorSet write = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
                    write.dstSet = descSet;
                    write.dstBinding = 0;
                    write.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
                    write.descriptorCount = 1;
                    write.pImageInfo = &lutInfo;
                    vkUpdateDescriptorSets(device, 1, &write, 0, nullptr);

                    {
                        VKComputePipeline lutPipeline(spv, { descLayout });
                        VulkanContext::Get().ImmediateSubmit([&](VkCommandBuffer cmd) {
                            lutPipeline.Bind(cmd);
                            vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE,
                                lutPipeline.GetLayout(), 0, 1, &descSet, 0, nullptr);
                            vkCmdDispatch(cmd, (lutSize + 15) / 16, (lutSize + 15) / 16, 1);
                        });
                    }

                    VulkanContext::Get().ImmediateSubmit([&](VkCommandBuffer cmd) {
                        TransitionImage(cmd, vkLut->GetImage(),
                            VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                            VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT,
                            VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 1, 1);
                    });

                    vkDestroyDescriptorSetLayout(device, descLayout, nullptr);
                }

                // envCubemap goes out of scope here — temp resource freed
                vkDestroyDescriptorPool(device, iblPool, nullptr);
            }

            // ---- 8. IBL sampler ----
            {
                VkSamplerCreateInfo sampCI{ VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO };
                sampCI.magFilter = VK_FILTER_LINEAR;
                sampCI.minFilter = VK_FILTER_LINEAR;
                sampCI.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
                sampCI.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
                sampCI.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
                sampCI.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
                sampCI.maxLod = 4.0f;
                vkCreateSampler(device, &sampCI, nullptr, &result.iblSampler);
            }

            // ---- 9. Skybox cube mesh + shader compilation ----
            {
                float cubeVertices[] = {
                    // +X
                     1, -1, -1,   1, -1,  1,   1,  1,  1,   1,  1,  1,   1,  1, -1,   1, -1, -1,
                    // -X
                    -1, -1,  1,  -1, -1, -1,  -1,  1, -1,  -1,  1, -1,  -1,  1,  1,  -1, -1,  1,
                    // +Y
                    -1,  1, -1,   1,  1, -1,   1,  1,  1,   1,  1,  1,  -1,  1,  1,  -1,  1, -1,
                    // -Y
                    -1, -1,  1,   1, -1,  1,   1, -1, -1,   1, -1, -1,  -1, -1, -1,  -1, -1,  1,
                    // +Z
                    -1, -1,  1,  -1,  1,  1,   1,  1,  1,   1,  1,  1,   1, -1,  1,  -1, -1,  1,
                    // -Z
                     1, -1, -1,   1,  1, -1,  -1,  1, -1,  -1,  1, -1,  -1, -1, -1,   1, -1, -1,
                };
                result.skyboxVB = std::make_shared<VKVertexBuffer>(cubeVertices, sizeof(cubeVertices));

                if (auto sh = ShaderLibrary::LoadEngine("shaders/skybox.vert"))
                    result.skyboxVertSpv = sh->GetSpirV();
                if (auto sh = ShaderLibrary::LoadEngine("shaders/skybox.frag"))
                    result.skyboxFragSpv = sh->GetSpirV();
                if (result.skyboxVertSpv.empty() || result.skyboxFragSpv.empty())
                    LH_CORE_ERROR("Failed to compile skybox shaders!");
            }

            LH_CORE_INFO("IBL: Precomputation complete (irradiance 32x32, prefiltered 128x128, BRDF LUT 512x512)");
            return result;
        }
    }
}
