// 为矩形发光面上的分层采样点建立六面深度投影，缓存静止场景的可见性数据。
#pragma once
#include "gpu_buffer.hpp"
#include "shadow_map.hpp"
#include <minirender/scene/scene.hpp>

class RectangleShadow {
  public:
    // 更新光源样本位置与场景包围盒对应的投影，返回是否更换了 GPU 资源句柄。
    bool configure(VulkanViewport& viewport, const minirender::SceneSnapshot& scene,
                   glm::vec3 minimum, glm::vec3 maximum, int grid, std::uint64_t revision);
    // 输入发生变化后调用 draw(layer, source_light)，每个数组层是一张独立深度图。
    void render(VkCommandBuffer                                        command,
                const std::function<void(std::uint32_t, std::size_t)>& draw);

    VkDescriptorImageInfo depth() const noexcept {
        return map_.descriptor();
    }

    VkDescriptorBufferInfo projections() const noexcept {
        return {matrices_.handle(), 0, VK_WHOLE_SIZE};
    }

    int grid() const noexcept {
        return grid_;
    }

    std::uint64_t updates() const noexcept {
        return updates_;
    }

    void release();
    static constexpr std::uint32_t resolution = 192;

  private:
    // 光照颜色与相机参数不改变深度。单独保存遮挡几何和灯面几何以复用深度图。
    struct Caster {
        std::shared_ptr<const minirender::MeshData> mesh;
        glm::mat4                                   transform;
        bool                                        double_sided;
        bool operator==(const Caster&) const = default;
    };

    std::vector<Caster>                   casters_;
    std::vector<std::array<glm::vec3, 4>> corners_;
    ShadowMap                             map_;
    GpuBuffer                             matrices_;
    std::uint32_t                         layers_   = 0;
    int                                   grid_     = 0;
    bool                                  dirty_    = false;
    std::uint64_t                         revision_ = ~std::uint64_t(0), updates_ = 0;
};
