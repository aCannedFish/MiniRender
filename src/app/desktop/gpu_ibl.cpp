#include "gpu_ibl.hpp"

#include <glm/vec4.hpp>
#include <stdexcept>

GpuFloatTexture::~GpuFloatTexture() {
    release();
}
void GpuFloatTexture::create(VulkanViewport& viewport, std::span<const minirender::Image> layers) {
    if (layers.empty() ||
        layers.size() > viewport.physicalDeviceProperties()->limits.maxImageArrayLayers) {
        throw std::invalid_argument("Unsupported IBL texture layer count");
    }
    release();
    device_ = viewport.device();
    layers_ = static_cast<std::uint32_t>(layers.size());
    const auto width = layers.front().width(), height = layers.front().height();
    image_.create(device_, viewport.physicalDevice(),
                  {static_cast<std::uint32_t>(width), static_cast<std::uint32_t>(height)},
                  VK_FORMAT_R32G32B32A32_SFLOAT,
                  VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                  VK_IMAGE_ASPECT_COLOR_BIT, 1, layers_);
    std::vector<glm::vec4> pixels;
    pixels.reserve(width * height * layers.size());
    for (std::uint32_t layer = 0; layer < layers_; ++layer) {
        const auto& image = layers[layer];
        if (image.width() != width || image.height() != height) {
            throw std::invalid_argument("IBL texture layers must have equal dimensions");
        }
        VkBufferImageCopy copy{};
        copy.bufferOffset = VkDeviceSize(pixels.size()) * sizeof(glm::vec4);
        copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, layer, 1};
        copy.imageExtent = {static_cast<std::uint32_t>(width), static_cast<std::uint32_t>(height),
                            1};
        copies_.push_back(copy);
        for (std::size_t y = 0; y < height; ++y) {
            for (std::size_t x = 0; x < width; ++x) {
                pixels.emplace_back(image.at(x, y), 1);
            }
        }
    }
    staging_.create(device_, viewport.physicalDevice(), pixels.size() * sizeof(glm::vec4),
                    VK_BUFFER_USAGE_TRANSFER_SRC_BIT);
    staging_.write(pixels.data(), pixels.size() * sizeof(glm::vec4));
    VkSamplerCreateInfo sampler{};
    sampler.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    sampler.magFilter = sampler.minFilter = VK_FILTER_NEAREST;
    sampler.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    sampler.addressModeU = sampler.addressModeV = sampler.addressModeW =
        VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    check_vk(vkCreateSampler(device_, &sampler, nullptr, &sampler_), "IBL sampler");
}
void GpuFloatTexture::record_upload(VkCommandBuffer command) {
    if (uploaded_) {
        return;
    }
    VkImageMemoryBarrier barrier{};
    barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    barrier.srcQueueFamilyIndex = barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    barrier.image = image_.image();
    barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, layers_};
    vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         0, 0, nullptr, 0, nullptr, 1, &barrier);
    vkCmdCopyBufferToImage(command, staging_.handle(), image_.image(),
                           VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                           static_cast<std::uint32_t>(copies_.size()), copies_.data());
    barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    barrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, nullptr, 0, nullptr, 1,
                         &barrier);
    uploaded_ = true;
}
void GpuFloatTexture::retire_upload() {
    if (uploaded_) {
        staging_.release();
    }
}
void GpuFloatTexture::release() {
    if (device_ != VK_NULL_HANDLE) {
        vkDestroySampler(device_, sampler_, nullptr);
    }
    image_.release();
    staging_.release();
    copies_.clear();
    device_ = VK_NULL_HANDLE;
    sampler_ = VK_NULL_HANDLE;
    layers_ = 0;
    uploaded_ = false;
}
VkDescriptorImageInfo GpuFloatTexture::descriptor() const noexcept {
    return {sampler_, image_.view(), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
}

void GpuIbl::upload_environment(VulkanViewport& viewport) {
    if (maps_) {
        diffuse_.create(viewport, {&maps_->diffuse, 1});
        specular_.create(viewport, maps_->specular);
        source_.create(viewport, *maps_->source);
    } else {
        const minirender::Image black(1, 1);
        diffuse_.create(viewport, {&black, 1});
        const std::array<minirender::Image, 2> layers{black, black};
        specular_.create(viewport, layers);
        source_.create(viewport, minirender::Texture(black));
    }
}
void GpuIbl::create(VulkanViewport& viewport) {
    upload_environment(viewport);
    const minirender::Image black(2, 2);
    brdf_.create(viewport, {&black, 1});
}
bool GpuIbl::update(VulkanViewport& viewport,
                    std::shared_ptr<const minirender::IblEnvironment> maps,
                    std::shared_ptr<const minirender::Image> lut) {
    const bool changed = maps != maps_ || lut != lut_;
    if (maps != maps_) {
        maps_ = std::move(maps);
        upload_environment(viewport);
    }
    if (lut != lut_) {
        lut_ = std::move(lut);
        const minirender::Image black(2, 2);
        brdf_.create(viewport, {lut_ ? lut_.get() : &black, 1});
    }
    return changed;
}
void GpuIbl::record_upload(VkCommandBuffer command) {
    diffuse_.retire_upload();
    specular_.retire_upload();
    brdf_.retire_upload();
    source_.retire_upload();
    diffuse_.record_upload(command);
    specular_.record_upload(command);
    brdf_.record_upload(command);
    source_.record_upload(command);
}
void GpuIbl::release() {
    diffuse_.release();
    specular_.release();
    brdf_.release();
    source_.release();
    maps_.reset();
    lut_.reset();
}
std::array<VkDescriptorImageInfo, 4> GpuIbl::descriptors() const noexcept {
    return {diffuse_.descriptor(), specular_.descriptor(), brdf_.descriptor(),
            source_.descriptor()};
}
