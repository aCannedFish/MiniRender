#include "diagnostic_renderer.hpp"

#include <QFile>

#include <array>
#include <cstring>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

// 附带具体操作名，把驱动错误作为显式失败传到 UI/测试进程。
void check(VkResult result, const char* operation) {
    if (result != VK_SUCCESS) {
        throw std::runtime_error(std::string(operation) + " failed: " + std::to_string(result));
    }
}

// 临时 shader module 的生命周期只覆盖 pipeline 创建；失败时也必须释放。
class ShaderModule {
  public:
    ShaderModule(VkDevice device, const QString& resource) : device_(device) {
        QFile file(resource);
        if (!file.open(QIODevice::ReadOnly)) {
            throw std::runtime_error("Cannot load embedded SPIR-V shader");
        }
        const QByteArray bytes = file.readAll();
        if (bytes.isEmpty() || bytes.size() % 4 != 0) {
            throw std::runtime_error("SPIR-V byte count is invalid");
        }
        std::vector<std::uint32_t> words(static_cast<std::size_t>(bytes.size()) / 4);
        std::memcpy(words.data(), bytes.constData(), static_cast<std::size_t>(bytes.size()));
        VkShaderModuleCreateInfo info{};
        info.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
        info.codeSize = static_cast<std::size_t>(bytes.size());
        info.pCode = words.data();
        check(vkCreateShaderModule(device_, &info, nullptr, &handle_), "create shader");
    }
    ~ShaderModule() {
        vkDestroyShaderModule(device_, handle_, nullptr);
    }
    ShaderModule(const ShaderModule&) = delete;
    ShaderModule& operator=(const ShaderModule&) = delete;
    VkShaderModule get() const {
        return handle_;
    }

  private:
    VkDevice device_;
    VkShaderModule handle_ = VK_NULL_HANDLE;
};

} // namespace

DiagnosticRenderer::DiagnosticRenderer(VulkanViewport* window) : window_(window) {}

std::uint32_t DiagnosticRenderer::memory_type(std::uint32_t bits,
                                              VkMemoryPropertyFlags flags) const {
    VkPhysicalDeviceMemoryProperties properties{};
    vkGetPhysicalDeviceMemoryProperties(window_->physicalDevice(), &properties);
    for (std::uint32_t index = 0; index < properties.memoryTypeCount; ++index) {
        if ((bits & (1U << index)) != 0 &&
            (properties.memoryTypes[index].propertyFlags & flags) == flags) {
            return index;
        }
    }
    throw std::runtime_error("Required Vulkan memory type is unavailable");
}

void DiagnosticRenderer::initResources() {
    device_ = window_->device();
    try {
        create_texture();
        create_descriptors();
    } catch (const std::exception& error) {
        window_->report_failure(QString::fromUtf8(error.what()));
    }
}

void DiagnosticRenderer::create_texture() {
    constexpr std::uint32_t side = 8;
    std::array<std::uint8_t, side * side * 4> pixels{};
    for (std::uint32_t y = 0; y < side; ++y) {
        for (std::uint32_t x = 0; x < side; ++x) {
            const bool cyan = (x + y) % 2 == 0;
            const auto offset = 4 * (y * side + x);
            pixels[offset] = cyan ? 30 : 230;
            pixels[offset + 1] = cyan ? 170 : 205;
            pixels[offset + 2] = cyan ? 210 : 130;
            pixels[offset + 3] = 255;
        }
    }

    VkBufferCreateInfo buffer_info{};

    buffer_info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    buffer_info.size = pixels.size();
    buffer_info.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    check(vkCreateBuffer(device_, &buffer_info, nullptr, &staging_), "create staging buffer");
    VkMemoryRequirements requirements{};
    vkGetBufferMemoryRequirements(device_, staging_, &requirements);
    VkMemoryAllocateInfo allocate{};
    allocate.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    allocate.allocationSize = requirements.size;
    allocate.memoryTypeIndex =
        memory_type(requirements.memoryTypeBits,
                    VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    check(vkAllocateMemory(device_, &allocate, nullptr, &staging_memory_), "allocate staging");
    check(vkBindBufferMemory(device_, staging_, staging_memory_, 0), "bind staging");
    void* mapped = nullptr;
    check(vkMapMemory(device_, staging_memory_, 0, pixels.size(), 0, &mapped), "map staging");
    std::memcpy(mapped, pixels.data(), pixels.size());
    vkUnmapMemory(device_, staging_memory_);

    VkImageCreateInfo image_info{};

    image_info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    image_info.imageType = VK_IMAGE_TYPE_2D;
    image_info.format = VK_FORMAT_R8G8B8A8_SRGB;
    image_info.extent = {side, side, 1};
    image_info.mipLevels = 1;
    image_info.arrayLayers = 1;
    image_info.samples = VK_SAMPLE_COUNT_1_BIT;
    image_info.tiling = VK_IMAGE_TILING_OPTIMAL;
    image_info.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    check(vkCreateImage(device_, &image_info, nullptr, &texture_), "create texture");
    vkGetImageMemoryRequirements(device_, texture_, &requirements);
    allocate.allocationSize = requirements.size;
    allocate.memoryTypeIndex =
        memory_type(requirements.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    check(vkAllocateMemory(device_, &allocate, nullptr, &texture_memory_), "allocate texture");
    check(vkBindImageMemory(device_, texture_, texture_memory_, 0), "bind texture");

    VkImageViewCreateInfo view_info{};

    view_info.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    view_info.image = texture_;
    view_info.viewType = VK_IMAGE_VIEW_TYPE_2D;
    view_info.format = image_info.format;
    view_info.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    check(vkCreateImageView(device_, &view_info, nullptr, &texture_view_), "create texture view");

    VkSamplerCreateInfo sampler_info{};

    sampler_info.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    sampler_info.magFilter = VK_FILTER_NEAREST;
    sampler_info.minFilter = VK_FILTER_NEAREST;
    sampler_info.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    sampler_info.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sampler_info.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sampler_info.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    check(vkCreateSampler(device_, &sampler_info, nullptr, &sampler_), "create sampler");
}

void DiagnosticRenderer::create_descriptors() {
    VkDescriptorSetLayoutBinding binding{};
    binding.binding = 0;
    binding.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    binding.descriptorCount = 1;
    binding.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    VkDescriptorSetLayoutCreateInfo layout_info{};
    layout_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    layout_info.bindingCount = 1;
    layout_info.pBindings = &binding;
    check(vkCreateDescriptorSetLayout(device_, &layout_info, nullptr, &descriptor_layout_),
          "create descriptor layout");
    VkDescriptorPoolSize size{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1};
    VkDescriptorPoolCreateInfo pool_info{};
    pool_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    pool_info.maxSets = 1;
    pool_info.poolSizeCount = 1;
    pool_info.pPoolSizes = &size;
    check(vkCreateDescriptorPool(device_, &pool_info, nullptr, &descriptor_pool_),
          "create descriptor pool");
    VkDescriptorSetAllocateInfo allocate{};
    allocate.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    allocate.descriptorPool = descriptor_pool_;
    allocate.descriptorSetCount = 1;
    allocate.pSetLayouts = &descriptor_layout_;
    check(vkAllocateDescriptorSets(device_, &allocate, &descriptor_), "allocate descriptor");
    VkDescriptorImageInfo image_info{sampler_, texture_view_,
                                     VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    VkWriteDescriptorSet write{};
    write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    write.dstSet = descriptor_;
    write.dstBinding = 0;
    write.descriptorCount = 1;
    write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    write.pImageInfo = &image_info;
    vkUpdateDescriptorSets(device_, 1, &write, 0, nullptr);
    VkPipelineLayoutCreateInfo pipeline_info{};
    pipeline_info.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    pipeline_info.setLayoutCount = 1;
    pipeline_info.pSetLayouts = &descriptor_layout_;
    check(vkCreatePipelineLayout(device_, &pipeline_info, nullptr, &pipeline_layout_),
          "create pipeline layout");
}

void DiagnosticRenderer::initSwapChainResources() {
    if (!window_->failure().isEmpty()) {
        return;
    }
    try {
        create_pipeline();
        window_->report_swapchain();
    } catch (const std::exception& error) {
        window_->report_failure(QString::fromUtf8(error.what()));
    }
}

void DiagnosticRenderer::create_pipeline() {
    ShaderModule vertex(device_, ":/shaders/diagnostic.vert.spv");
    ShaderModule fragment(device_, ":/shaders/diagnostic.frag.spv");
    std::array<VkPipelineShaderStageCreateInfo, 2> stages{};
    for (auto& stage : stages) {
        stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stage.pName = "main";
    }
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = vertex.get();
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = fragment.get();
    VkPipelineVertexInputStateCreateInfo vertex_input{};
    vertex_input.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    VkPipelineInputAssemblyStateCreateInfo assembly{};
    assembly.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    assembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    VkPipelineViewportStateCreateInfo viewport{};
    viewport.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    viewport.viewportCount = 1;
    viewport.scissorCount = 1;
    VkPipelineRasterizationStateCreateInfo raster{};
    raster.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    raster.polygonMode = VK_POLYGON_MODE_FILL;
    raster.cullMode = VK_CULL_MODE_NONE;
    raster.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    raster.lineWidth = 1.0F;
    VkPipelineMultisampleStateCreateInfo multisample{};
    multisample.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    VkPipelineDepthStencilStateCreateInfo depth{};
    depth.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
    depth.depthTestEnable = VK_TRUE;
    depth.depthWriteEnable = VK_TRUE;
    depth.depthCompareOp = VK_COMPARE_OP_LESS;
    VkPipelineColorBlendAttachmentState attachment{};
    attachment.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                                VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    VkPipelineColorBlendStateCreateInfo blend{};
    blend.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    blend.attachmentCount = 1;
    blend.pAttachments = &attachment;
    const std::array<VkDynamicState, 2> states{VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dynamic{};
    dynamic.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
    dynamic.dynamicStateCount = static_cast<std::uint32_t>(states.size());
    dynamic.pDynamicStates = states.data();
    VkGraphicsPipelineCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    info.stageCount = static_cast<std::uint32_t>(stages.size());
    info.pStages = stages.data();
    info.pVertexInputState = &vertex_input;
    info.pInputAssemblyState = &assembly;
    info.pViewportState = &viewport;
    info.pRasterizationState = &raster;
    info.pMultisampleState = &multisample;
    info.pDepthStencilState = &depth;
    info.pColorBlendState = &blend;
    info.pDynamicState = &dynamic;
    info.layout = pipeline_layout_;
    info.renderPass = window_->defaultRenderPass();
    check(vkCreateGraphicsPipelines(device_, VK_NULL_HANDLE, 1, &info, nullptr, &pipeline_),
          "create graphics pipeline");
}

void DiagnosticRenderer::record_texture_upload(VkCommandBuffer command) {
    // 首帧 upload 与 draw 在同一 graphics queue；barrier 保证拷贝写入先于采样读取。
    VkImageMemoryBarrier barrier{};
    barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = texture_;
    barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         0, 0, nullptr, 0, nullptr, 1, &barrier);
    VkBufferImageCopy copy{};
    copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    copy.imageExtent = {8, 8, 1};
    vkCmdCopyBufferToImage(command, staging_, texture_, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1,
                           &copy);
    barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    barrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, nullptr, 0, nullptr, 1,
                         &barrier);
    uploaded_ = true;
}

void DiagnosticRenderer::startNextFrame() {
    if (!window_->failure().isEmpty()) {
        window_->frameReady();
        return;
    }
    const auto command = window_->currentCommandBuffer();
    if (!uploaded_) {
        record_texture_upload(command);
    }
    const QSize size = window_->swapChainImageSize();
    const VkExtent2D extent{static_cast<std::uint32_t>(size.width()),
                            static_cast<std::uint32_t>(size.height())};
    std::array<VkClearValue, 2> clear{};
    clear[0].color = {{0.012F, 0.018F, 0.028F, 1.0F}};
    clear[1].depthStencil = {1.0F, 0};
    VkRenderPassBeginInfo begin{};
    begin.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    begin.renderPass = window_->defaultRenderPass();
    begin.framebuffer = window_->currentFramebuffer();
    begin.renderArea.extent = extent;
    begin.clearValueCount = static_cast<std::uint32_t>(clear.size());
    begin.pClearValues = clear.data();
    vkCmdBeginRenderPass(command, &begin, VK_SUBPASS_CONTENTS_INLINE);
    VkViewport viewport{
        0.0F, 0.0F, static_cast<float>(extent.width), static_cast<float>(extent.height),
        0.0F, 1.0F};
    VkRect2D scissor{{0, 0}, extent};
    vkCmdSetViewport(command, 0, 1, &viewport);
    vkCmdSetScissor(command, 0, 1, &scissor);
    vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_);
    vkCmdBindDescriptorSets(command, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_layout_, 0, 1,
                            &descriptor_, 0, nullptr);
    vkCmdDraw(command, 3, 2, 0, 0);
    vkCmdEndRenderPass(command);
    window_->report_frame();
    window_->frameReady();
    window_->requestUpdate();
}

void DiagnosticRenderer::releaseSwapChainResources() {
    // VulkanViewport 在回收 swapchain 前等待相关帧完成；pipeline 不再被在途命令引用。
    if (pipeline_ != VK_NULL_HANDLE) {
        vkDestroyPipeline(device_, pipeline_, nullptr);
        pipeline_ = VK_NULL_HANDLE;
    }
}

void DiagnosticRenderer::releaseResources() {
    if (device_ == VK_NULL_HANDLE) {
        return;
    }
    releaseSwapChainResources();
    vkDestroyPipelineLayout(device_, pipeline_layout_, nullptr);
    vkDestroyDescriptorPool(device_, descriptor_pool_, nullptr);
    vkDestroyDescriptorSetLayout(device_, descriptor_layout_, nullptr);
    vkDestroySampler(device_, sampler_, nullptr);
    vkDestroyImageView(device_, texture_view_, nullptr);
    vkDestroyImage(device_, texture_, nullptr);
    vkFreeMemory(device_, texture_memory_, nullptr);
    vkDestroyBuffer(device_, staging_, nullptr);
    vkFreeMemory(device_, staging_memory_, nullptr);
    pipeline_layout_ = VK_NULL_HANDLE;
    descriptor_pool_ = VK_NULL_HANDLE;
    descriptor_layout_ = VK_NULL_HANDLE;
    sampler_ = VK_NULL_HANDLE;
    texture_view_ = VK_NULL_HANDLE;
    texture_ = VK_NULL_HANDLE;
    texture_memory_ = VK_NULL_HANDLE;
    staging_ = VK_NULL_HANDLE;
    staging_memory_ = VK_NULL_HANDLE;
    device_ = VK_NULL_HANDLE;
    uploaded_ = false;
}
