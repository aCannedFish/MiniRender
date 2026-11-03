#pragma once

#include "gpu_buffer.hpp"
#include "gpu_texture.hpp"
#include "hdr_targets.hpp"
#include "post_process.hpp"
#include "shadow_map.hpp"
#include "viewport_renderer.hpp"
#include "vulkan_viewport.hpp"

#include <limits>
#include <map>

// 三角网格的实时直接光照管线。使用场景快照，UI 修改通过 revision 发布。
// 光照写入浮点 HDR；法线、深度与显示变换由独立后处理消费。
class SceneRenderer final : public ViewportRenderer {
  public:
    explicit SceneRenderer(VulkanViewport* window);
    void initResources() override;
    void initSwapChainResources() override;
    void releaseSwapChainResources() override;
    void releaseResources() override;
    void startNextFrame() override;
    void request_hdr_capture() override;
    minirender::Image read_hdr_capture() override;

  private:
    VkPipeline create_pipeline(bool shadow);
    void render_shadow_maps(VkCommandBuffer command);
    void upload_scene();
    void rebuild_texture_descriptors();
    using MeshHandle = std::shared_ptr<const minirender::MeshData>;
    using TextureHandle = std::shared_ptr<const minirender::Texture>;
    // 网格保留局部坐标。变换与材质写入独立 SSBO，不再烘焙到每个顶点。
    struct Mesh {
        GpuBuffer vertices;
        std::uint32_t count = 0;
        glm::vec3 minimum{0};
        glm::vec3 maximum{0};
    };
    struct Draw {
        MeshHandle mesh;
        TextureHandle texture;
        VkDescriptorSet descriptor = VK_NULL_HANDLE;
    };
    std::map<MeshHandle, Mesh, std::owner_less<MeshHandle>> meshes_;
    std::vector<Draw> draws_;
    TextureHandle white_texture_;
    std::map<TextureHandle, std::unique_ptr<GpuTexture>, std::owner_less<TextureHandle>> textures_;
    void update_uniforms();
    VulkanViewport* window_;
    VkDevice device_ = VK_NULL_HANDLE;
    ShadowMap shadow_map_;
    HdrTargets targets_;
    PostProcess post_process_;
    bool capture_hdr_ = false;
    std::array<glm::mat4, 8> shadow_matrices_{};
    GpuBuffer instances_;
    std::size_t instance_capacity_ = 0;
    GpuBuffer uniforms_;
    VkDescriptorSetLayout descriptor_layout_ = VK_NULL_HANDLE;
    VkDescriptorPool descriptor_pool_ = VK_NULL_HANDLE;
    VkDescriptorSet descriptor_ = VK_NULL_HANDLE;
    VkPipelineLayout pipeline_layout_ = VK_NULL_HANDLE;
    VkPipeline pipeline_ = VK_NULL_HANDLE;
    VkPipeline shadow_pipeline_ = VK_NULL_HANDLE;
    std::uint64_t scene_revision_ = std::numeric_limits<std::uint64_t>::max();
};
