#include "sdf_renderer.hpp"
#include "gpu_shader.hpp"

#include <glm/gtc/matrix_transform.hpp>

#include <array>
#include <cmath>
#include <cstddef>

namespace {

// std140：矩阵 64 字节，三份 vec4 共 48 字节。节点图和帧数据共 7360 字节。
struct SdfGpuNode {
    glm::mat4 inverse_transform{1};
    glm::vec4 parameters{0};
    glm::vec4 color{0};
    glm::ivec4 links{0};
};
struct SdfGpuFrame {
    glm::vec4 origin{0}, forward{0}, right{0}, up{0};
    glm::vec4 bounds_min{0}, bounds_max{0}, settings{0};
    glm::ivec4 counts{0};
    glm::vec4 light_direction{0}, light_color{0}, ambient{0};
    glm::ivec4 display{0};
    std::array<SdfGpuNode, 64> nodes;
};
struct DisplayPush {
    float exposure;
    std::int32_t surface;
};
static_assert(sizeof(SdfGpuNode) == 112 && offsetof(SdfGpuNode, links) == 96);
static_assert(sizeof(SdfGpuFrame) == 7360 && offsetof(SdfGpuFrame, nodes) == 192);
static_assert(sizeof(DisplayPush) == 8);

} // namespace

SdfRenderer::SdfRenderer(VulkanViewport* viewport) : viewport_(viewport) {}

void SdfRenderer::initResources() {
    device_ = viewport_->device();
    uniform_.create(device_, viewport_->physicalDevice(), sizeof(SdfGpuFrame),
                    VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT);
}

void SdfRenderer::initSwapChainResources() {
    trace_.create(*viewport_, VK_FORMAT_R32G32B32A32_SFLOAT, ":/shaders/sdf.frag.spv",
                  uniform_.handle(), sizeof(SdfGpuFrame), {}, ":/shaders/sdf.vert.spv", 6);
    create_display();
}

void SdfRenderer::update_frame() {
    const auto& document = viewport_->sdf_document();
    if (!document) {
        throw std::logic_error("SDF view requires a document");
    }
    if (document != uploaded_) {
        field_ = std::make_unique<minirender::SdfField>(*document);
        uploaded_ = document;
        viewport_->report_sdf_upload();
    }
    const auto& camera = viewport_->camera();
    const auto& trace = document->trace;
    const auto position = glm::dvec3(camera.position);
    const auto forward = glm::normalize(glm::dvec3(camera.target) - position);
    const auto right = glm::normalize(glm::cross(forward, glm::dvec3(camera.up)));
    const double half_height = std::tan(glm::radians(double(camera.vertical_fov_degrees)) * .5);
    const auto size = viewport_->swapChainImageSize();
    SdfGpuFrame frame;
    frame.origin.w = camera.near_plane;
    frame.forward = {forward, camera.far_plane};
    frame.right = {right, half_height * size.width() / size.height()};
    frame.up = {glm::cross(right, forward), half_height};
    // 在 double 中将变换和平面范围移至相机原点，避免大坐标的 float 相减。
    const auto relative = glm::translate(glm::dmat4(1), position);
    frame.bounds_min = {field_->bounds().minimum - position, field_->bounds().empty ? 1 : 0};
    frame.bounds_max = {field_->bounds().maximum - position, trace.maximum_distance};
    frame.settings = {trace.epsilon, trace.relative_epsilon, trace.normal_step,
                      trace.safety_factor};
    frame.counts = {field_->nodes().size(), field_->root(), trace.maximum_steps,
                    trace.shadow_steps};
    frame.light_direction = {glm::normalize(glm::dvec3(document->light.direction)),
                             document->light.intensity};
    frame.light_color = {document->light.color, 0};
    frame.ambient = {document->ambient, 0};
    frame.display = {static_cast<int>(viewport_->sdf_view()), viewport_->sdf_shadows() ? 1 : 0,
                     size.width(), size.height()};
    for (std::size_t i = 0; i < field_->nodes().size(); ++i) {
        const auto& node = field_->nodes()[i];
        auto& packed = frame.nodes[i];
        packed.inverse_transform = glm::mat4(node.world_to_local * relative);
        packed.parameters = {node.parameters, node.scale};
        packed.color = {node.color, 1};
        packed.links = {static_cast<int>(node.kind), static_cast<int>(node.left),
                        static_cast<int>(node.right), 0};
    }
    uniform_.write(&frame, sizeof(frame));
}

void SdfRenderer::create_display() {
    VkSamplerCreateInfo sampler{};
    sampler.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    sampler.magFilter = sampler.minFilter = VK_FILTER_NEAREST;
    sampler.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    sampler.addressModeU = sampler.addressModeV = sampler.addressModeW =
        VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    check_vk(vkCreateSampler(device_, &sampler, nullptr, &sampler_), "SDF display sampler");
    const VkDescriptorSetLayoutBinding binding{0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1,
                                               VK_SHADER_STAGE_FRAGMENT_BIT, nullptr};
    VkDescriptorSetLayoutCreateInfo descriptors{};
    descriptors.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    descriptors.bindingCount = 1;
    descriptors.pBindings = &binding;
    check_vk(vkCreateDescriptorSetLayout(device_, &descriptors, nullptr, &descriptors_),
             "SDF display descriptor layout");
    const VkDescriptorPoolSize pool_size{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1};
    VkDescriptorPoolCreateInfo pool{};
    pool.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    pool.maxSets = pool.poolSizeCount = 1;
    pool.pPoolSizes = &pool_size;
    check_vk(vkCreateDescriptorPool(device_, &pool, nullptr, &pool_), "SDF display pool");
    VkDescriptorSetAllocateInfo allocate{};
    allocate.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    allocate.descriptorPool = pool_;
    allocate.descriptorSetCount = 1;
    allocate.pSetLayouts = &descriptors_;
    check_vk(vkAllocateDescriptorSets(device_, &allocate, &descriptor_), "SDF display descriptor");
    const VkDescriptorImageInfo image{sampler_, trace_.view(),
                                      VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    VkWriteDescriptorSet write{};
    write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    write.dstSet = descriptor_;
    write.descriptorCount = 1;
    write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    write.pImageInfo = &image;
    vkUpdateDescriptorSets(device_, 1, &write, 0, nullptr);
    const VkPushConstantRange push{VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(DisplayPush)};
    VkPipelineLayoutCreateInfo layout{};
    layout.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    layout.setLayoutCount = layout.pushConstantRangeCount = 1;
    layout.pSetLayouts = &descriptors_;
    layout.pPushConstantRanges = &push;
    check_vk(vkCreatePipelineLayout(device_, &layout, nullptr, &layout_), "SDF display layout");
    GpuShader vertex(device_, ":/shaders/sdf.vert.spv");
    GpuShader fragment(device_, ":/shaders/sdf_display.frag.spv");
    std::array<VkPipelineShaderStageCreateInfo, 2> stages{};
    for (auto& stage : stages) {
        stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stage.pName = "main";
    }
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = vertex.handle();
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = fragment.handle();
    VkPipelineVertexInputStateCreateInfo input{};
    input.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    VkPipelineInputAssemblyStateCreateInfo assembly{};
    assembly.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    assembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    VkPipelineViewportStateCreateInfo view{};
    view.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    view.viewportCount = view.scissorCount = 1;
    VkPipelineRasterizationStateCreateInfo raster{};
    raster.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    raster.polygonMode = VK_POLYGON_MODE_FILL;
    raster.cullMode = VK_CULL_MODE_NONE;
    raster.lineWidth = 1;
    VkPipelineMultisampleStateCreateInfo multisample{};
    multisample.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    VkPipelineDepthStencilStateCreateInfo depth{};
    depth.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
    VkPipelineColorBlendAttachmentState attachment{};
    attachment.colorWriteMask = 0xF;
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
    info.pVertexInputState = &input;
    info.pInputAssemblyState = &assembly;
    info.pViewportState = &view;
    info.pRasterizationState = &raster;
    info.pMultisampleState = &multisample;
    info.pDepthStencilState = &depth;
    info.pColorBlendState = &blend;
    info.pDynamicState = &dynamic;
    info.layout = layout_;
    info.renderPass = viewport_->defaultRenderPass();
    check_vk(vkCreateGraphicsPipelines(device_, VK_NULL_HANDLE, 1, &info, nullptr, &pipeline_),
             "SDF display pipeline");
}

void SdfRenderer::display() {
    const auto command = viewport_->currentCommandBuffer();
    const auto size = viewport_->swapChainImageSize();
    const VkExtent2D extent{static_cast<std::uint32_t>(size.width()),
                            static_cast<std::uint32_t>(size.height())};
    std::array<VkClearValue, 2> clear{};
    clear[1].depthStencil = {1, 0};
    VkRenderPassBeginInfo begin{};
    begin.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    begin.renderPass = viewport_->defaultRenderPass();
    begin.framebuffer = viewport_->currentFramebuffer();
    begin.renderArea.extent = extent;
    begin.clearValueCount = static_cast<std::uint32_t>(clear.size());
    begin.pClearValues = clear.data();
    vkCmdBeginRenderPass(command, &begin, VK_SUBPASS_CONTENTS_INLINE);
    const VkViewport view{0, 0, static_cast<float>(extent.width), static_cast<float>(extent.height),
                          0, 1};
    const VkRect2D scissor{{0, 0}, extent};
    vkCmdSetViewport(command, 0, 1, &view);
    vkCmdSetScissor(command, 0, 1, &scissor);
    vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_);
    vkCmdBindDescriptorSets(command, VK_PIPELINE_BIND_POINT_GRAPHICS, layout_, 0, 1, &descriptor_,
                            0, nullptr);
    const DisplayPush push{viewport_->exposure(),
                           viewport_->sdf_view() == minirender::SdfView::surface ? 1 : 0};
    vkCmdPushConstants(command, layout_, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(push), &push);
    vkCmdDraw(command, 6, 1, 0, 0);
    vkCmdEndRenderPass(command);
}

void SdfRenderer::capture() {
    const auto size = viewport_->swapChainImageSize();
    const auto command = viewport_->currentCommandBuffer();
    const auto bytes = VkDeviceSize(size.width()) * VkDeviceSize(size.height()) * sizeof(glm::vec4);
    readback_.create(device_, viewport_->physicalDevice(), bytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT);
    VkImageMemoryBarrier barrier{};
    barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    barrier.srcQueueFamilyIndex = barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = trace_.image();
    barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    barrier.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    barrier.oldLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                         VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &barrier);
    VkBufferImageCopy copy{};
    copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    copy.imageExtent = {static_cast<std::uint32_t>(size.width()),
                        static_cast<std::uint32_t>(size.height()), 1};
    vkCmdCopyImageToBuffer(command, trace_.image(), VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                           readback_.handle(), 1, &copy);
    barrier.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    barrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, nullptr, 0, nullptr, 1,
                         &barrier);
    captured_ = true;
    capture_requested_ = false;
}

void SdfRenderer::startNextFrame() {
    update_frame();
    trace_.draw(*viewport_);
    if (capture_requested_) {
        capture();
    }
    display();
    viewport_->report_frame();
    viewport_->frameReady();
    viewport_->requestUpdate();
}

void SdfRenderer::request_hdr_capture() {
    capture_requested_ = true;
}

minirender::Image SdfRenderer::read_hdr_capture() {
    if (!captured_) {
        throw std::logic_error("No SDF HDR capture was recorded");
    }
    const auto size = viewport_->swapChainImageSize();
    std::vector<glm::vec4> pixels(static_cast<std::size_t>(size.width()) *
                                  static_cast<std::size_t>(size.height()));
    readback_.read(pixels.data(), pixels.size() * sizeof(glm::vec4));
    minirender::Image image(static_cast<std::size_t>(size.width()),
                            static_cast<std::size_t>(size.height()));
    for (std::size_t y = 0; y < image.height(); ++y) {
        for (std::size_t x = 0; x < image.width(); ++x) {
            image.at(x, y) = glm::vec3(pixels[y * image.width() + x]);
        }
    }
    return image;
}

void SdfRenderer::releaseSwapChainResources() {
    if (!device_) {
        return;
    }
    vkDestroyPipeline(device_, pipeline_, nullptr);
    vkDestroyPipelineLayout(device_, layout_, nullptr);
    vkDestroyDescriptorPool(device_, pool_, nullptr);
    vkDestroyDescriptorSetLayout(device_, descriptors_, nullptr);
    vkDestroySampler(device_, sampler_, nullptr);
    pipeline_ = VK_NULL_HANDLE;
    layout_ = VK_NULL_HANDLE;
    pool_ = VK_NULL_HANDLE;
    descriptors_ = VK_NULL_HANDLE;
    descriptor_ = VK_NULL_HANDLE;
    sampler_ = VK_NULL_HANDLE;
    trace_.release();
    readback_.release();
    captured_ = false;
}

void SdfRenderer::releaseResources() {
    releaseSwapChainResources();
    uniform_.release();
    uploaded_.reset();
    field_.reset();
    device_ = VK_NULL_HANDLE;
}
