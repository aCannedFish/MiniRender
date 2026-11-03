#include "gpu_texture.hpp"

#include <vector>

GpuTexture::~GpuTexture() {
    release();
}

void GpuTexture::create(VulkanViewport& viewport, const minirender::Texture& texture) {
    release();
    device_ = viewport.device();
    levels_ = static_cast<std::uint32_t>(texture.level_count());
    const auto& base = texture.level(0);
    image_.create(
        device_, viewport.physicalDevice(),
        {static_cast<std::uint32_t>(base.width()), static_cast<std::uint32_t>(base.height())},
        VK_FORMAT_R32G32B32A32_SFLOAT, VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
        VK_IMAGE_ASPECT_COLOR_BIT, levels_);
    std::vector<float> pixels;
    for (std::uint32_t level = 0; level < levels_; ++level) {
        const auto& source = texture.level(level);
        VkBufferImageCopy region{};
        region.bufferOffset = VkDeviceSize(pixels.size()) * sizeof(float);
        region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, level, 0, 1};
        region.imageExtent = {static_cast<std::uint32_t>(source.width()),
                              static_cast<std::uint32_t>(source.height()), 1};
        regions_.push_back(region);
        for (std::size_t y = 0; y < source.height(); ++y) {
            for (std::size_t x = 0; x < source.width(); ++x) {
                const auto color = source.at(x, y);
                pixels.insert(pixels.end(), {color.r, color.g, color.b, 1.0F});
            }
        }
    }
    staging_.create(device_, viewport.physicalDevice(), pixels.size() * sizeof(float),
                    VK_BUFFER_USAGE_TRANSFER_SRC_BIT);
    staging_.write(pixels.data(), pixels.size() * sizeof(float));
    VkSamplerCreateInfo sampler{};
    sampler.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    sampler.magFilter = VK_FILTER_NEAREST;
    sampler.minFilter = VK_FILTER_NEAREST;
    sampler.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    sampler.addressModeU = sampler.addressModeV = sampler.addressModeW =
        VK_SAMPLER_ADDRESS_MODE_REPEAT;
    sampler.maxLod = static_cast<float>(levels_ - 1);
    check_vk(vkCreateSampler(device_, &sampler, nullptr, &sampler_), "create material sampler");
}

void GpuTexture::record_upload(VkCommandBuffer command) {
    if (uploaded_) {
        return;
    }
    VkImageMemoryBarrier barrier{};
    barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    barrier.image = image_.image();
    barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, levels_, 0, 1};
    vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         0, 0, nullptr, 0, nullptr, 1, &barrier);
    vkCmdCopyBufferToImage(command, staging_.handle(), image_.image(),
                           VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                           static_cast<std::uint32_t>(regions_.size()), regions_.data());
    barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    barrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, nullptr, 0, nullptr, 1,
                         &barrier);
    uploaded_ = true;
}
void GpuTexture::retire_upload() {
    if (uploaded_) {
        staging_.release();
    }
}
void GpuTexture::release() {
    if (device_ != VK_NULL_HANDLE) {
        vkDestroySampler(device_, sampler_, nullptr);
    }
    image_.release();
    staging_.release();
    sampler_ = VK_NULL_HANDLE;
    device_ = VK_NULL_HANDLE;
    uploaded_ = false;
    regions_.clear();
}
VkDescriptorImageInfo GpuTexture::descriptor() const noexcept {
    return {sampler_, image_.view(), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
}
